# Plan — IrModule: minimal IR receiver peripheral that adjusts brightness + palette

## Context

The SE16 / LightCrafter boards carry an IR receiver (SE16: GPIO 5, shared with Ethernet MISO
via the board switch; LightCrafter: GPIO 4). "IR" is currently a `planned` capability. The
product owner wants a **minimal IR service** that actually *does* something — adjust global
**brightness** — rather than only exposing a raw code. The full remote-code → action mapping is
a later step; for now the action plumbing is proven with **`brightness up` / `brightness down`
buttons** in the UI, wired to the same brightness-adjust path a decoded IR code will call later.

Confirmed with the product owner:
- **Receive only** (no TX).
- **Peripheral, catalog-wired** — factory-registered like AudioModule / I2cScanModule, added
  per board via `deviceModels.json`; NOT a hardcoded child of System.
- **Start minimal, grow later** — brightness up/down now; richer remote mapping is a follow-up.

Backlog alignment ([backlog-mixed.md](../../../backlog/backlog-mixed.md)): IR is named as an input
for the eventual **LightsControl** hub. This module is the thin IR *input* peripheral; when
LightsControl is built it consumes IR via the same static seam (the `AudioModule::latestFrame()`
pattern). This does not build LightsControl — it builds the IR input and one concrete action.

## Files

1. **New `src/core/IrModule.h`** — shaped on `I2cScanModule` (the minimal peripheral template):
   - `class IrModule : public MoonModule`; `role() → Peripheral`; `respectsEnabled()` default
     (true — IR is a real feature, not a diagnostic).
   - Controls: `addPin("pin", pin_)` (IR receiver GPIO), `addButton("brightness up")`,
     `addButton("brightness down")`, `addReadOnly("last code", codeStr_)` (shows the last
     decoded code once the RMT decode lands; blank while the seam is a stub).
   - `onUpdate(name)`: `"brightness up"` → `Drivers::adjustBrightness(+kStep)`; `"brightness
     down"` → `Drivers::adjustBrightness(-kStep)`. `kStep = 16` (a perceptible notch; 16 steps
     across the 0–255 range).
   - `loop()`: poll `platform::irRead(pin_, code)`; on a fresh code, store it for the readout
     (and later: map to an action). Stub returns false today, so loop is a cheap no-op.
   - Static `latestCode()` seam for a future LightsControl consumer (mirrors
     `AudioModule::latestFrame()`), returning the last decoded code (0 = none yet).

2. **`src/light/drivers/Drivers.h`** — add a static brightness-adjust seam:
   - `static void adjustBrightness(int delta);` — clamps `brightness` to [0,255], rebuilds the
     correction LUT, and notifies driver children via `onCorrectionChanged()` — the SAME path
     `onUpdate("brightness")` takes, so an IR-driven change behaves exactly like the UI slider.
   - Needs a static instance pointer (`static Drivers* active_;` set in `setup()`, cleared in
     `teardown()`) — the established single-owner seam pattern. No-op if no Drivers is live.

3. **`src/platform/platform.h`** — new seam near `i2cScan`:
   - `bool irRead(uint16_t pin, uint32_t& codeOut);` — true when a fresh IR frame decodes on
     `pin` (self-contained: opens/owns its RMT-RX channel, like `i2cScan` opens its own bus).
     Present-tense doc: today ESP32 + desktop both stub to `false`; the RMT-NEC decode is a
     focused follow-up (avoids rushing RMT-vs-RmtLedDriver channel contention into this cut).

4. **`src/platform/esp32/platform_esp32*.cpp`** + **`platform_desktop.cpp`** — `irRead` stubs
   returning `false`. (ESP32's real RMT-NEC decode is the follow-up; the RMT-RX machinery
   already exists in `platform_esp32_rmt.cpp`.)

5. **`web-installer/deviceModels.json`** — add an `IrModule` child to SE16 (`pin: 5`) and
   LightCrafter (`pin: 4`). Keep "IR" in `planned` (NOT `supported`) until the ESP32 decode
   lands — per the vocabulary rule, `supported` requires a working backing capability, and the
   *decode* isn't working yet even though the module + brightness action are. The brightness
   buttons work now; IR reception is the planned part.

6. **`src/main.cpp`** — `registerType<mm::IrModule>("IrModule", "core/IrModule.md")`.

7. **Docs** — the per-module technical page is **moxygen-generated** from `IrModule.h`'s `///`
   comments (gitignored, not hand-written); the hand-authored piece is a summary entry in
   `docs/moonmodules/core/ui/ui.md` linking to `../moxygen/IrModule.md`, plus embedding the
   remote photo effects.md-style (`<img … width="300">`). *(Superseded the original line below,
   which planned a hand-written `IrModule.md` — corrected to the moxygen/ui.md convention.)* The
   old line: `docs/moonmodules/core/IrModule.md` — cross-file wiring + the brightness seam + the
   "Prior art" note (NEC IR protocol, ESP-IDF RMT RX example).

8. **Test** `test/unit/core/unit_IrModule.cpp` — the brightness buttons adjust `Drivers`
   brightness (up clamps at 255, down clamps at 0, step size); a stub `irRead` yields no code.
   Driven through the public control path like `unit_AudioModule_sync`.

## What actually shipped (grew past the original "brightness buttons" scope)

The cut ended up delivering the full receive + learn feature, live-proven on the SE16 and
LightCrafter:

- **Real RMT NEC decode** (`platform_esp32_ir.cpp`): a persistent RX channel on `pin`, an
  ISR-minimal done-callback (signals a queue only — no decode/re-arm in interrupt context, the
  same discipline as rmtWs2812RxCapture), decode + re-arm on the render task in `irRead`.
  Live-proven: 4 distinct remote buttons → 4 stable 32-bit codes on both boards.
- **Learned code → action mapping.** A `learn` select arms an action; the next received code
  binds to it (stored per-action in a persistent `code …` Text control, rebuilt into a fast
  `learnedCode_` lookup on load). A received code runs its bound action. PO decision
  (2026-07-04): learning (any remote, live) over MoonLight's fixed per-remote presets.
- **No UI action buttons** (PO decision 2026-07-04): the remote is the interface once learned, so
  brightness/palette up-down buttons would duplicate it — removed. The `learn` select + per-action
  `code …` read-outs are the whole UI.
- **No `last code` control** (PO decision 2026-07-04): the received code shows in the status line
  ("received 0x…" / "learned … = 0x…"), so a separate read-out would duplicate it.
- **Status feedback** via base `MoonModule::setStatus` (+ a per-module `statusBuf_` for dynamic
  text, the I2cScan/Devices pattern): "set pin to receive" / "ready" (setup), "learning: press a
  remote button" (armed), "learned <action> = 0x…" (bound), "Drivers.brightness → N" (fired),
  "received 0x… (unassigned)" (unbound code).

## Scope guards (held)

- **No IR transmit.**
- **No LightsControl hub** — this is the IR input peripheral only; `latestCode()` is the seam it
  will consume.

## Follow-up (next session)

- **`effect next` / `effect prev`** — change the effect in layer[0] slot[0]. PO asked for it
  (2026-07-04); deferred because it is a module *replace* (swap the effect type), not a
  `setControl` nudge. It needs: (a) a **replace-by-type primitive extracted to Scheduler** (the
  same move as the setControl extraction — currently `applyReplace` is HttpServer-private), (b)
  ordered **effect-type enumeration by role** from ModuleFactory, (c) tree navigation to the
  layer[0] slot. Then it's two more `kActions`-style rows that call the replace primitive instead
  of setControl. Design the primitive first.
- **Move "IR" `planned` → `supported`** in the catalog now that decode works on hardware (a
  `supported` capability must have a working backing module — it does now).

## Verification (done)

- Desktop + all ESP32 variants build clean (`-Werror`); `ctest` (9 IrModule cases: learn/bind,
  fire, clamp, independent bindings, unassigned, robustness); scenarios green; `check_specs` /
  `check_devices` valid.
- **Live on SE16 (GPIO 5, IR/Eth switch) and LightCrafter (GPIO 4, IR+Eth simultaneous)**: remote
  press decodes to a code, learn binds it, a bound code drives brightness/palette. Boards boot
  clean with the decoder (the first ISR-unsafe version crash-looped; the task-side rewrite is
  stable).
