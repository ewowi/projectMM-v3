# Plan — Shift-register (74HCT595) LED driver

## Context

A **74HCT595 shift-register expander board** turns each physical data GPIO into **8 outputs**. The PO owns two S3-N16R8-driven panels — **15×256** (3,840 lights) and **48×256** (12,288 lights) — not yet wired. Goal: drive them from the drivers we already have, not a new driver class.

The feasibility research is [`docs/history/shift-register-driver-analysis.md`](../../shift-register-driver-analysis.md). The two facts that shape this plan:

1. **The ×8 fan-out costs 8× the DMA frame** (~145 KB for *both* targets — extra strands ride the bus width and are free; the ×8 rides the serial shift and is not). Confirmed from hpwit's sizing expressions and his 8.00× clock ratio.
2. **The 8× bus clock is granted by `esp_lcd`** — bench-confirmed 2026-07-14 on S3 **and** P4: `request 20000000 Hz -> GRANTED (prescale 4 -> granted 20000000 Hz)`.

**Scope: the i80/LCD_CAM path — S3 *and* P4.** Both grant the 20 MHz clock (bench-confirmed), both reach PSRAM, so both are in from the start; there is no extra cost to including the P4 and no reason to exclude it. Classic ESP32 is out **for now** — its DMA cannot reach PSRAM, so ~145 KB hits a ~76 KB internal wall; it comes in when the parked **PSRAM refill ring** (a many-small-buffers memory model) lands, and the driver must refuse shift mode there with a clear status until then. Parlio is out (65,535-byte one-shot cap; the chunked-transfer backlog item is its route). RMT is structurally impossible (self-timed NRZ, no clock line).

## The mechanism

A '595 is **serial-in, parallel-out**: 8 bits take 8 clock cycles. So the shift encode replaces each WS2812 data slot with **8 shift cycles**, and the peripheral's own pixel clock (WR) is the shift clock.

Per WS2812 bit, direct vs shift mode:

| | slots per bit | data slot |
|---|---|---|
| today | 3 | one bus word, bit L = physical lane L |
| **shift mode** | **3 × 8 = 24** | 8 shift cycles, each a bus word whose bit L = physical pin L, carrying **strand (cycle, L)** |

`transposeLanes8x8` stays the primitive — each shift cycle is still an 8-lane bit-plane transpose, just gathering a different set of 8 strands. **The shift encoder wraps the existing SWAR core; it does not replace it.**

**Pin cost = `dataPins + 2`.** hpwit's "120 outputs from 15 pins" is really 17 GPIOs:
- **CLOCK** — peripheral-driven (`LCD_PCLK_IDX`), **zero DMA bytes**. This is our existing i80 `clockPin` (WR), a genuinely lucky fit: it already exists and already does the right thing.
- **LATCH** — a **data lane** in the bus word (a bit in every slot). *This* is what forces the extra DMA words, and it consumes one of the 8 bus bits.

## The clock: ask for 20 MHz, not hpwit's 19.2

`esp_lcd` derives an **integer** prescale from the bus resolution and **silently rounds down** — it errors only when the prescale is 0 or > `LCD_LL_PCLK_DIV_MAX` (64). A wrong clock is therefore **not an error, it is a wrong waveform**, so the rate must divide exactly.

S3 and P4 both: `LCD_CLK_SRC_PLL160M` ÷ `LCD_PERIPH_CLOCK_PRE_SCALE` (2) = **80 MHz** bus resolution.

| | prescale | granted |
|---|---|---|
| today (`kPclkHz`) | 30 | 2.667 MHz (exact) |
| **shift mode** | **4** | **20.000 MHz (exact)** |

hpwit's 19.2 MHz is an artifact of the classic **I2S fractional** divider (`div_num`/`div_a`/`div_b`), which LCD_CAM does not have. 8 × 2.667 = 21.3 MHz has no exact divisor; 20 MHz does. Same reasoning that already picked the exact `/30`. (P4 also exposes `LCD_CLK_SRC_APLL` — an escape hatch if the 3-slot timing ever needs true 21.33 MHz.)

## Module shape: a checkbox on the existing drivers, not a new module

**Two controls on `ParallelLedDriver`** (the shared CRTP base), so **i80 and Parlio both inherit it** and `I80LedDriver` needs ~nothing:

- `outputsPerPin` (1 = direct, 8 = a 74HCT595 per pin) — 1 by default
- `latchPin` (GPIO) — shown only when the expander is on (the existing conditional-control pattern)

The ×8 is a **hardware constant**, not a user control (question 3 answered: the physical board is ×8).

Why this shape, against the principles:
- ***Default to subtraction*** — no new module, no new driver class, no new registration. Two controls and one encoder branch.
- ***Common patterns first*** — a mode flag that changes a driver's output encoding is the ordinary shape.
- The repo's **own bottom-up analysis independently concluded** *"the multiplex is a configuration of a parallel-clocked backend, not a sibling driver class."* Three passes now agree.

## Implementation

### 1. `src/light/drivers/ParallelSlots.h` — the shift encoder
Add `encodeWs2812ShiftSlots<Slot>()` beside the existing encoder. Same 3-slot structure, but each data slot becomes 8 shift cycles; reuses `transposeLanes8x8` per cycle. The latch bit is set in the bus word on the cycle that presents the byte. **Pure data transform, no platform include** — so it is pinned by a host unit test with no ESP32, exactly like the current encoder.

### 2. `src/light/drivers/ParallelLedDriver.h` — the plumbing
- Two controls (`outputsPerPin`, `latchPin`), both `affectsPrepare`.
- `frameBytesFor(...)` gains an `outputsPerPin` factor (1 or 8).
- `encodeRows<Slot>` branches to the shift encoder when engaged.
- `parseConfig` maps strands → physical pins (`ceil(lanes / 8)`), and **validates `latchPin`** against the data pins and `clockPin` (a collision is a config error with a status, not a crash — *Robust to any input*).

### 3. `src/platform/esp32/platform_esp32_i80.cpp` — the clock
`i80Ws2812Init` takes the pclk as a parameter (or a shift-mode flag) so the bus opens at **20 MHz** in shift mode and 2.667 MHz otherwise. This is the *only* platform change, and the spike proved it is granted.

### 4. Guards (the honest failure modes)
- **Memory** — ~145 KB must allocate. It fits PSRAM on N16R8; on failure the existing **allocate-and-degrade** path already idles the driver with a status. No new mechanism.
- **Not on classic / Parlio** — the driver must **refuse shift mode with a clear status** on a backend that can't do it, rather than producing a broken waveform. (Classic: whole-frame internal DMA won't hold 145 KB. Parlio: 65,535-byte cap.)

## Verification

**Host (no hardware — where the real proof is):**
- `unit_ParallelSlots` — extend with the shift encoder: bit-exact expected slot streams for a known input, the latch bit asserted on the right cycles, inactive lanes idle LOW, ×8 fan-out gathers the right strand per cycle. This is the piece that *can* be fully pinned without the '595 board, so it carries the weight.
- `frameBytesFor` with `outputsPerPin = 8` → the ~145 KB arithmetic, and the 15×256 == 48×256 equality (the counter-intuitive result worth a test so it can't silently regress).
- A `latchPin` colliding with a data pin or `clockPin` → config error, no crash.

**Hardware (S3, once the '595 board exists):**
- The **loopback self-test works in shift mode**, and it is a *stronger* test than the direct-mode one: it verifies the whole chain — encode → i80 bus → shift register → latch → output — where direct mode only ever proved the ESP32 half. (This plan originally assumed "shift mode changes the encoding, not the harness"; that was wrong — the harness needed the ×8 frame size, the shift encoder, the shift pclk, and the continuity pre-check skipped.)

  **The jumper moves.** In direct mode you wire a data GPIO to `loopbackRxPin`. In shift mode a data GPIO carries the 20 MHz serial stream *into* the '595, not pixel data — so the wire comes off the register's **output** side. See § "Loopback wiring" for the exact pin and the **5 V level-shift hazard**.
- Then the real panels: 15×256, then 48×256.

**The one thing no test can pre-empt:** whether a real WS2812 strand latches correctly off a 20 MHz bus through a '595. That is a *hardware* property, and it needs the board. It is the residual risk of this plan, and it is deliberately not hidden.

## Scope guards

The i80/LCD_CAM path: **S3 + P4**. NOT classic (needs the parked PSRAM refill ring — add it there when that lands). NOT Parlio (65,535-byte cap). NOT RMT (impossible). NOT ×16 cascaded '595s (doubles the buffer again; ×8 is what the physical board does). No new module, no new driver class.

## Open risk

The ~145 KB frame is confirmed by arithmetic, not yet by an allocation on the S3 at that size in shift mode. It is well inside the 16,384-light frame the S3 already drives today (~150 KB), so this is expected to be a non-event — but it is an assumption until the first `prepare()` runs.
