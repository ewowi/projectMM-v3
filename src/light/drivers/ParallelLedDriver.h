#pragma once

#include "light/drivers/DriverBase.h"        // DriverBase, Correction
#include "light/drivers/LedPeripheral.h"     // runtime peripheral strategy (Parlio / i80 / MoonI80)
#include "light/drivers/ParallelSlots.h"        // encodeWs2812ParallelSlots (shared encoder)
#include "light/drivers/LedDriverConfig.h"
#include "light/drivers/PinList.h"         // parsePinList / assignCounts (shared)
#include "platform/platform.h"

#include <numeric>  // std::gcd — the snapshot split's cache-line alignment

namespace mm {

/// The one registered parallel WS2812B LED-output driver. It drives up to 16 strands that clock out
/// SIMULTANEOUSLY, one GPIO lane each, fed consecutive slices of the source buffer (see kMaxLanes), over
/// whichever bus peripheral the `peripheral` control selects at runtime (the S3's LCD_CAM i80 bus, its
/// own-GDMA MoonI80 shift-ring, or the P4's Parlio peripheral — see the LedPeripheral strategy below).
/// (Vocabulary — strand / lane / slot / row — under @xref{terminology|More info → Terminology}.)
///
/// **How it works: encode the whole frame, then hand it to the DMA and walk away.** Every WS2812 bit of
/// every strand is written into one buffer up front — including a zeroed ≥300 µs pad at the end, the
/// reset that tells the strands the frame is over — and then shipped as a single autonomous transfer.
/// The encode is a **fused correct + transpose, per row** (ParallelSlots.h; the transpose mechanism is
/// under @xref{the-frame-transpose-correct-transpose-per-row|More info → the frame transpose}).
///
/// **Why single-shot matters:** there is NO CPU deadline while a frame is on the wire. A driver that
/// refills buffers as the DMA drains them must beat the clock on every refill, and a WiFi interrupt at
/// the wrong moment slips a bit and garbles the rest of the frame. Encoding first makes that impossible
/// by construction. (The streaming ring in MoonLedDriver deliberately gives this up — it is the price of
/// a frame too big to hold.)
///
/// **Slicing:** the strands are fed consecutive slices of this driver's window, in `pins` order, sized
/// by `ledsPerPin` with the remainder split evenly — the same rule (and the same parser) as
/// RmtLedDriver, so a strand count means the same thing on every LED driver.
///
/// **A runtime `LedPeripheral` strategy, not compile-time CRTP.** The base calls into `peripheral_`
/// (a `LedPeripheral*`, see LedPeripheral.h) through one virtual-dispatch boundary per FRAME or per
/// reinit — never per light, so the per-light encode (the actual hot path) stays a direct call and pays
/// no vtable cost. A peripheral supplies only the variant-specific pieces: the `bus*` platform wrappers,
/// `lanesAvailable()` (which makes it inert on the wrong chip), `powerOfTwoBus()` (the i80 bus rounds to
/// 8 or 16; Parlio's width IS its pin count), and any extra bus pins it owns. Each backend header
/// (I80Peripheral, MoonI80Peripheral, ParlioPeripheral) self-registers its factory + label with the
/// peripheral registry at static-init; which backends link is `#if`-gated per chip's CONFIG_SOC_* at the
/// backend `#include`s in main.cpp, so a board links only its usable backends and the `peripheral`
/// control offers that subset. A fresh driver wires the first usable backend so it is
/// functional out of the box; the control (or a catalog `peripheral` value) switches it live.
///
/// @moreinfo
///
/// ## Terminology
///
/// A **strand** is one chain of LEDs. A **lane** is one bus data line — in the normal case, one GPIO
/// driving one strand. A **slot** is one WS2812 bit on the wire, and a **row** is one light across every
/// strand at once (so a frame is `maxLaneLights` rows).
///
/// ## The frame transpose (correct + transpose, per row)
///
/// The source has each light's bytes together, but the wire needs each bus WORD to carry one bit of EVERY
/// strand at the same instant (bit L of the word = data line L). So the encoder turns 8 lights' bytes on
/// their side — an 8x8 bit matrix transpose — and writes one word per slot, fused with the per-light
/// correction in one pass (ParallelSlots.h). Both peripherals take the identical bytes: a Parlio bus word
/// and an i80 bus word have the same meaning.
class ParallelLedDriver : public DriverBase {
public:
    /// Test-only: swap in a peripheral backend for a test mock. Deinits + drops any existing peripheral
    /// first (a rebuild-from-scratch, same as a real reinit would need). The caller retains ownership —
    /// this borrows the pointer, exactly like a registered driver's own owned backend does not need a
    /// test to manage its lifetime.
    void setPeripheralForTest(LedPeripheral* p) {
        if (peripheral_ && peripheralOwned_) { peripheral_->busDeinit(); delete peripheral_; }
        else if (peripheral_) peripheral_->busDeinit();
        peripheral_ = p;
        peripheralOwned_ = false;   // the test owns a stack/heap mock; the orchestrator must not delete it
        peripheralActiveReg_ = 0xFF;
        if (p) p->attach(this);
    }

    /// Push the heap total to the module readout. Production calls this from the alloc/free sites
    /// (publishHeapBytes); a test that drives a mock peripheral's busInit directly has no such site,
    /// so it asks for the recompute here and then asserts the PUBLIC dynamicBytes() the card shows.
    void publishHeapBytesForTest() { publishHeapBytes(); }

    // --- Peripheral backend registry ---
    // The set of peripheral backends compiled into THIS build. Each backend header self-registers its
    // factory + label at static-init, via an `inline const bool kXxxPeripheralRegistered =
    // registerPeripheral(...)` at the bottom of the backend header (e.g. MoonLedDriver.h, ParlioLedDriver.h).
    // WHICH backends link is decided in main.cpp: the `#include` of each backend header is `#if`-gated on
    // the chip's CONFIG_SOC_* — so a classic ESP32 only links the esp_lcd-i80 backend, an S3 links i80 +
    // MoonI80, a P4 links all three. The `peripheral` Select then offers the linked-and-supported subset
    // at runtime. Mirrors ModuleFactory's static-registration shape, one level down for the bus backend.
    using PeripheralFactory = LedPeripheral* (*)();
    struct PeripheralEntry { const char* label; PeripheralFactory make; };
    static constexpr uint8_t kMaxPeripherals = 4;
    static inline PeripheralEntry peripheralRegistry_[kMaxPeripherals] = {};
    static inline uint8_t peripheralRegistryCount_ = 0;
    /// Register a backend factory under a UI label. Called once per linked backend at static-init.
    /// Duplicate labels and overflow past kMaxPeripherals are ignored (the backstop is far above the
    /// three real backends).
    static bool registerPeripheral(const char* label, PeripheralFactory make) {
        if (!label || !make || peripheralRegistryCount_ >= kMaxPeripherals) return false;
        for (uint8_t i = 0; i < peripheralRegistryCount_; i++)
            if (std::strcmp(peripheralRegistry_[i].label, label) == 0) return false;
        peripheralRegistry_[peripheralRegistryCount_++] = {label, make};
        return true;
    }

    /// WS2812/SK6812 strips are GRB-wired, so a fresh parallel LED driver references the "GRB" preset by
    /// default (the user can pick any). A fresh driver also wires the first peripheral this chip supports
    /// so it is functional out of the box; the `peripheral` control (or a catalog `peripheral` value)
    /// switches it. On desktop no backend is linked, so peripheral_ stays null and the driver idles —
    /// tests inject a mock via setPeripheralForTest.
    ParallelLedDriver() {
        this->setDefaultPresetName("GRB");
        selectDefaultPeripheral(nullptr);
    }

    /// Free the owned backend. A registered driver's backend (created via the registry) is owned and
    /// deleted here; a test-borrowed mock (setPeripheralForTest) is not — the ownership flag decides,
    /// so the delete lives ONCE in the base rather than per thin subclass. The owner released()/removed
    /// this driver before destruction (the DriverBase vptr-race contract), so the bus is already down.
    ~ParallelLedDriver() override {
        if (peripheral_ && peripheralOwned_) delete peripheral_;
        peripheral_ = nullptr;
    }

    /// Max parallel lanes = the peripheral's full 16 data lines. The bus width the driver
    /// actually builds is DERIVED from the configured pin count: ≤8 pins → an 8-bit bus (uint8
    /// slots), 9..16 pins → a 16-bit bus (uint16 slots). Both peripherals do 16 data lines
    /// (LCD_CAM `bus_width` 8|16; Parlio `data_width` 8|16 — powers of two only).
    /// This is the BUS WIDTH — a hardware fact. The number of STRANDS can exceed it when a
    /// shift-register expander is fitted (see kMaxStrands).
    static constexpr uint8_t kMaxLanes = 16;

    /// Max strands the driver can drive: every data line fanned out through its '595 chain. Sizes the
    /// per-strand arrays (laneCounts_, laneStart_). In direct mode only the first kMaxLanes are used.
    ///
    /// **64 is a REAL ceiling, and it is the active-strand mask that sets it.** The mask is a
    /// `uint64_t` (encodeWs2812ShiftSlots' `activeMask`), so one row carries at most 64 strands.
    /// parseConfig rejects anything larger rather than silently dropping strands.
    ///
    /// What that allows and blocks — worth knowing before wiring a board:
    ///
    /// | config | strands | frame (256 lights) | |
    /// |---|---|---|---|
    /// | 6 pins ×8  | 48  | ~151 KB | ✅ hpwit's board today |
    /// | 7 pins ×8  | 56  | ~151 KB | ✅ |
    /// | 15 pins ×8 | 120 | ~301 KB | ❌ over the mask (hpwit's headline number) |
    ///
    /// **Lifting it is cheap and NOT blocked by the uint64_t** — that framing would be wrong. The
    /// encoder only ever needs *one physical pin's* strands at a time (it indexes
    /// `p * outputsPerPin() + pos`), so a **per-pin mask of ≤16 bits** removes the ceiling entirely
    /// without ever making the hot-path test a multi-word object. Deliberately not done yet: no board
    /// we have needs >56, and the encode loop is still being profiled (it is the fps bottleneck), so
    /// this is queued rather than guessed at.
    ///
    /// Extra **pins** ride the bus width and are nearly free; extra **shift depth** (cascading '595s)
    /// would double the DMA frame *and* the shift time. hpwit's 120 comes from 15 PINS × 8, not from
    /// cascading. **More pins beats deeper cascades** — which, with the clock arithmetic in
    /// ParallelSlots.h, is why only the ×8 wiring is offered at all.
    static constexpr uint8_t kMaxStrands = 64;

    /// Light count the loopback self-test drives (or `maxLaneLights_` if the strip is smaller).
    /// Small on purpose: the test verifies the peripheral emits *correct WS2812 bits*, which a few
    /// hundred lights prove fully (encode, fused correct+transpose, single-shot DMA, latch pad) — it
    /// does NOT need the operational grid. A big frame hits two hardware limits: the P4 Parlio rejects
    /// a single non-loop transfer over `PARLIO_LL_TX_MAX_BITS_PER_FRAME` (~0.5 Mbit), and the RMT-RX
    /// capture can't hold a large symbol count (a 128×128 grid = 16384 lights is ~1.2 Mbit TX / ~400k
    /// capture symbols → transfer rejected AND capture returns nothing, surfacing as a misleading "bad
    /// bit 0"). 256 lights = ~18 KB TX / ~6144 capture symbols — comfortably inside both on every
    /// parallel driver, so the test runs identically at any grid size.
    static constexpr nrOfLightsType kLoopbackTestLights = 256;

    /// Comma-separated GPIO list, one parallel lane per pin — up to kMaxLanes strands clocked out
    /// SIMULTANEOUSLY, fed consecutive slices of this driver's window. A token may be a single pin or an
    /// inclusive range ("20-23" → 20,21,22,23), mixing freely ("20-22,35,38-40"). Shared control shape with
    /// RmtLedDriver (parsers in PinList.h). Defaults live on the derived (chip-specific safe pins),
    /// so the derived sets them after construction; the base just declares them. Both backends take
    /// 1..16 pins: the i80 BUS WIDTH rounds to 8 or 16 (powerOfTwoBus) and parks the unused lanes plus
    /// WR/DC on spare GPIOs, which is invisible to the pin list; Parlio's width is the pin count, so
    /// nothing rounds. Sized for 16 two-digit GPIOs + separators.
    char pins[64] = "";
    /// Comma-separated lights-per-lane; the unassigned remainder splits evenly over the remaining
    /// lanes. **A lane is a STRAND, not a pin** — which only differ through the expander: direct
    /// mode has one strand per pin, so entry N is pin N's; with `pinExpander` on, each pin fans
    /// out to 8 strands, so the list runs over all `pins × 8` of them (pin 0's eight first, then
    /// pin 1's, …) and entry N is strand N. That is what lets two strands on one '595 have
    /// different lengths. Each lane is clamped to the WS2812 per-pin ceiling
    /// (`kMaxWs2812LedsPerPin`) with a Warning status — it drives that many rather than choking a
    /// whole grid onto one line. Empty default splits this driver's window evenly. Sized for the full
    /// kMaxStrands contract: each strand's value is at most kMaxWs2812LedsPerPin (2048 → 4 digits) plus a
    /// separator, times kMaxStrands, plus the NUL — so a fully-explicit 64-strand list never truncates.
    char ledsPerPin[kMaxStrands * 5 + 1] = "";

    /// Async double-buffer transmit (default ON). When ON, the driver encodes the next frame into a
    /// SECOND DMA buffer while the current one clocks out (deferred-wait — per-tick wall-clock
    /// `max(encode, wire)` instead of `encode + wire`), so the blocking WS2812 wire wait no longer
    /// stalls the render thread. Measured on the P4 at 16×256 lights: it lifts the whole board from
    /// ~48 to ~76 fps (the driver tick drops from ~10.8 ms to ~3.8 ms of CPU — the ~7.7 ms wire moves
    /// into background DMA).
    ///
    /// **ON is simply the better configuration — the switch exists to A/B it, not because some setups
    /// should run it OFF.** The one-frame output latency it adds (~8–20 ms) sits inside the perceptual
    /// audio↔visual sync window and is small next to what the pipeline already spends (FFT window +
    /// render + the 8–16 ms WS2812 wire itself), so there is no user class (audio-reactive included)
    /// that should turn it off for latency. Leave it ON and take the fps.
    ///
    /// **It is per-driver because the resource is per-driver:** each driver's second DMA buffer lives
    /// on its own peripheral, sized by its own lane count and strand length (a 16-lane i80 frame and a
    /// 1-lane RMT strand are wildly different allocations), so a board can afford it for one driver and
    /// not another. If the buffer won't fit, the driver degrades to the synchronous path by itself
    /// rather than failing — the flag is the *measurement* knob; the degrade is automatic.
    ///
    /// Toggling rebuilds the bus (via affectsPrepare) to add or free the second buffer. Distinct from
    /// the container's `multicore` control, and they stack: this hides the WIRE behind DMA within one
    /// core; that hides the ENCODE behind the render on the other core. See docs/history/lessons.md.
    bool doubleBuffer = true;
    /// Streaming-ring source snapshot on/off — an A/B measurement knob, ring path only. ON (default,
    /// the safe behavior) freezes the source into a driver-owned buffer each frame so the ring's refill
    /// (which encodes off the render thread, concurrently with rendering) reads an immutable copy — no
    /// use-after-free on a grid resize, no frame tearing. OFF reads the live source directly, matching
    /// the pre-snapshot behavior, so a bench can A/B the snapshot's effect on ring frame completion live
    /// without a reflash. Live (NOT a prepare trigger): it changes only what tickRing does per frame,
    /// nothing the bus was built with, so the next tick honors the new value. Ignored off the ring path
    /// (the whole-frame paths encode inline on the render thread, no concurrency, so they never snapshot).
    bool ringSnapshot = true;
    /// On-device loopback self-test — jumper a lane's TX to `loopbackRxPin`, tick to transmit a
    /// known WS2812 pattern and bit-verify the capture, proving the peripheral emits correct bytes
    /// on real silicon. A persistent on/off mode (see onControlChanged): while on it re-runs on every
    /// relevant change; off clears the verdict. The verdict lands in the status slot.
    bool     loopbackTest = false;
    /// Optional TX override for the self-test: when set (>= 0), the loopback transmits on THIS pin
    /// in place of lane 0 (laneList_[0]); other lanes are unchanged. Lets the bench loopback run on
    /// a dedicated jumper pin without re-typing the operational `pins`. Falls back to laneList_[0]
    /// when unset (-1). The loopback only drives lane 0 with the test pattern, so a single override
    /// pin is all it needs. Test-only — normal output always uses the real `pins`. int8_t + addPin
    /// (not uint16): the standard single-GPIO control, and -1 = unset keeps GPIO 0 usable as a
    /// loopback pin.
    int8_t   loopbackTxPin = -1;
    /// Which STRAND carries the test pattern (shift mode only; ignored in direct mode, where the
    /// pattern always rides lane 0).
    ///
    /// With a 74HCT595 expander the jumper comes off a register OUTPUT, and which output is
    /// physically easiest to reach varies by board — while which STRAND that output carries depends
    /// on the '595's shift order, which is exactly the kind of thing that is easy to get wrong on
    /// paper. So rather than force the wire to strand 0, point the TEST at the wire: tap any output,
    /// set this to the strand it drives, and the self-test puts its pattern there. A mismatch then
    /// shows up as a bit FAIL, and walking this control 0..N-1 until the test PASSES *identifies*
    /// the strand empirically — turning "which output is Q7?" from a guess into a measurement.
    uint8_t  loopbackStrand = 0;
    /// Loopback mode: OFF (default) tears the output down and transmits a PRIVATE test frame on a rebuilt
    /// bus — proves the peripheral in isolation, but on the ring path it must re-allocate the whole pool,
    /// which fails on a fragmented heap, and it tests a replica. ON rides the LIVE pipeline: it pins the
    /// pattern into the running source, arms the RX on the wire the pipeline already drives, and verifies —
    /// no teardown, no alloc, and it sees the real render (a scattered ring shows as a bit fault at a slice
    /// boundary). Driver-agnostic; the coming loopbackMode dropdown folds this + loopbackTest into one.
    bool     loopbackIntrusive = false;
    /// Jumper this to the TX lane for the self-test (unset = -1 by default).
    ///
    /// **With a 74HCT595 expander (`pinExpander` on) the jumper comes from a DIFFERENT place, and
    /// getting it wrong can damage the ESP32.** In direct mode you jumper a data GPIO straight to
    /// this pin. In shift mode a data GPIO carries the fast serial stream *into* the register, not
    /// pixel data — so the wire must come from the register's OUTPUT side instead:
    ///
    ///  - **Which output:** the test pattern is driven on the strand named by **`loopbackStrand`** (which
    ///    defaults to 0 but can be any strand — pick the '595 output that is physically easiest to reach,
    ///    e.g. a spare one that drives no panel). Strand S is data pin `S / 8`'s register at shift
    ///    position `S % 8`. A '595 shifts MSB-first — the first bit clocked in lands on the LAST output —
    ///    so shift position `p` appears on output **Q(7 − p)**. Strand 0 (position 0) is therefore Q7;
    ///    strand 7 (position 7) is Q0; and so on. Feed that output back.
    ///  - **⚠ LEVEL-SHIFT IT.** The '595 runs at 5 V, so its output swings to **5 V**, and an ESP32 GPIO
    ///    is **not 5 V tolerant** — a direct wire can damage the pin. Use a divider (e.g. 1 kΩ from the
    ///    output to the GPIO, 2 kΩ from the GPIO to GND, giving 5 V × 2/3 ≈ 3.3 V) or any 5 V→3.3 V shifter.
    ///  - **No continuity pre-check runs in shift mode** — it would drive the TX pin and expect this
    ///    pin to follow, which a shift register does not do (that takes 8 clocks + a latch). So a
    ///    mis-wired jumper shows up as a bit FAIL, not as "jumper not detected".
    ///
    /// The payoff is a *stronger* test than direct mode: it verifies the whole chain — encode → i80
    /// bus → shift register → latch → output — rather than only the ESP32 half.
    int8_t   loopbackRxPin = -1;

    /// Is a 74HCT595 shift-register expander board fitted? OFF (the default) = strands wired straight
    /// to the GPIOs; ON = each data pin feeds one '595, so the driver drives `pins` × 8 strands.
    ///
    /// A **checkbox, not a fan-out number**: the 8 is the '595's own width, not a free parameter, so
    /// there are exactly two wirings and a boolean says which one the board has. (An earlier
    /// `outputsPerPin` number invited half-configured values like 4 and, bound as a Select, stored the
    /// menu *index* — which made `1` mean *eight* in a catalog entry.)
    ///
    /// The expander buys pins, not memory: the '595 is serial-in, so presenting 8 bits costs 8 shift
    /// cycles, and the DMA frame grows ×8 (each WS2812 slot becomes 8 bus words). Extra lanes on an
    /// already-fanned-out pin are then free (they ride the bus width) — which is why 15×256 and
    /// 48×256 cost the SAME frame. The bus also clocks ×8 faster (see kShiftPclkHz).
    /// Needs a `latchPin` and the LCD_CAM i80 path (ESP32-S3 / -P4); refused with a status elsewhere.
    ///
    /// **⚠️ EXPERIMENTAL, and size-limited today — the limit depends on the backend.** The ×8 frame is
    /// ~1.1 KB/light, so it exceeds internal DMA RAM (~110 KB, less once WiFi has taken its share) above
    /// roughly **96 lights per strand**. The **esp_lcd i80** backend must stream one contiguous frame, so
    /// it is capped there (above it the frame falls to PSRAM, which the S3's GDMA cannot sustain at the
    /// expander's 26.67 MHz clock — the frame never completes). The **MoonI80** backend lifts that with a
    /// streaming ring (small internal buffers refilled as the DMA drains them, so PSRAM is never on the
    /// path): it drives **128 lights/strand reliably**. Above 128 the ring reuses buffers and a residual
    /// refill-vs-DMA race still stalls it — the current working ceiling is 128/strand via MoonI80, 96 via
    /// i80.
    ///
    /// Full status, the reuse-race + concurrency follow-ups, and the measurements behind these limits:
    /// [the analysis](https://github.com/MoonModules/projectMM/blob/main/docs/history/shift-register-driver-analysis.md)
    /// and the ring items in `docs/backlog/backlog-light.md`.
    bool     pinExpander = false;
    /// The 74HCT595 LATCH (RCLK) line — pulsed once the shifted byte is in, presenting it on the
    /// '595 outputs. Unlike the shift clock (the peripheral's own WR pin), this is a DATA lane:
    /// it occupies one bit of every bus word, so it must NOT collide with a data pin or clockPin.
    /// Shown only when the expander is on. Unset (-1) until the user wires it.
    int8_t   latchPin = -1;

    /// Is the shift-register expander engaged? (Reads the control; the alias keeps the intent
    /// readable at the call sites that ask "am I encoding through a register?")
    // The EFFECTIVE expander mode: the user's saved `pinExpander` AND a peripheral that can host the
    // '595. Gating here (not by mutating `pinExpander`) means a peripheral that can't host it degrades to
    // direct mode at runtime while the saved value survives — so switching back to a supporting
    // peripheral restores shift mode. Every consumer (lane math, latch, ring, encode) reads this.
    bool pinExpanderMode() const { return pinExpander && peripheral_ && peripheral_->supportsPinExpander(); }
    /// Strands driven per physical data pin: 1 direct, or the '595's width through the expander.
    /// The multiplier every size/clock/slot computation below is expressed in. Reads the EFFECTIVE mode
    /// (pinExpanderMode), so on a peripheral that can't host the '595 the lane math is direct even though
    /// the saved `pinExpander` value is kept — matching every other consumer.
    uint8_t outputsPerPin() const { return pinExpanderMode() ? kPinExpanderOutputs : uint8_t(1); }

    /// Bind the driver's controls: the peripheral-INVARIANT block first (window, `pins`, `ledsPerPin`,
    /// `frameTime` — they describe *which LEDs and how many*, the same on any backend), THEN the
    /// `peripheral` selector as a divider, THEN the peripheral-SPECIFIC controls the chosen backend
    /// surfaces (`doubleBuffer` where supported, i80's clockPin/dcPin, the `pinExpander` toggle, MoonI80's
    /// ring cluster). The loopback cluster is last (expert). This ordering makes the card read top-down:
    /// everything above `peripheral` is invariant; everything below is what that peripheral supports.
    void defineDriverControls() override {
        addWindowControls();   // start / count — the slice of the shared buffer this driver outputs
        controls_.addText("pins", pins, sizeof(pins));
        controls_.addText("ledsPerPin", ledsPerPin, sizeof(ledsPerPin));
        // `doubleBuffer` is peripheral-DEPENDENT (a peripheral that can't run the async path hides it —
        // MoonI80), so it lives BELOW the peripheral divider with the other peripheral-specific controls,
        // not up here in the invariant block. Added after the swap; see below.
        // ringSnapshot is added in the derived driver's addRingControls() (below the useRing path selector,
        // with the rest of the ring cluster) — it is a ring-only knob, so it belongs with the ring
        // controls, not up here with the path-neutral ones.
        // Read-only KPI: the measured DMA wire time + the fps ceiling it implies. The pure output
        // floor (independent of render load), so it shows how much headroom remains as the pipeline
        // improves — and it reflects an overclocked slot rate directly. Refreshed in tick1s().
        controls_.addReadOnly("frameTime", frameTimeStr_, sizeof(frameTimeStr_));
        // The peripheral selector — the divider between the invariant block above and the
        // peripheral-specific controls below. buildPeripheralOptions syncs peripheralSel_ to the live
        // backend (or clamps it); when a peripheral CHANGE triggered this rebuild, peripheralSel_ already
        // holds the NEW index (applyControlValue wrote it before rebuildControls), so
        // ensurePeripheralMatchesSelection() makes the live object agree BEFORE addBusControls/addRingControls
        // surface the chosen backend's controls. Always shown, even with a single option: with one backend
        // it reads as a labeled indicator of what's driving the LEDs (e.g. "i80" on the classic).
        buildPeripheralOptions();
        ensurePeripheralMatchesSelection();
        controls_.addSelect("peripheral", peripheralSel_, peripheralOptions_, peripheralOptionCount_);
        // doubleBuffer — the first peripheral-specific control, directly under the divider. Peripheral-
        // dependent: a peripheral that can't run the async path hides it (MoonI80 — single-buffer only,
        // its own-GDMA two-buffer handshake races; see supportsDoubleBuffer / its busInit). Added AFTER
        // ensurePeripheralMatchesSelection so a peripheral CHANGE evaluates the visibility against the NEW
        // backend. The saved value stays bound, so it survives a round-trip through a single-buffer-only
        // peripheral and re-engages when a peripheral that supports it is selected again.
        controls_.addControl("doubleBuffer", doubleBuffer);
        controls_.setHidden(controls_.count() - 1, !(peripheral_ && peripheral_->supportsDoubleBuffer()));
        // The bus pins sit UNDER the peripheral selector because they ARE peripheral-specific: for a driver
        // that owns its own GPIO routing they are '595 pins: MoonI80 routes WR only when a shift register
        // needs it as SRCLK, and hides the control otherwise. (I80 goes through esp_lcd, which mandates a
        // valid WR *and* DC GPIO whatever the mode, so it shows both unconditionally — same hook, different answer.)
        if (peripheral_) peripheral_->addBusControls(controls_);
        // The 74HCT595 pin expander toggle + its latch pin. It comes BEFORE the ring cluster (below)
        // because that cluster DEPENDS on it — the ring/shift controls are shown only when the expander
        // is on, so the switch that controls them must sit above them (the dependent-below-controlling
        // rule). Peripheral-specific — shown only when the SELECTED peripheral can host the '595 (i80 /
        // MoonI80 on LCD_CAM; not Parlio, whose single-shot transfer can't carry the ×8 fan-out frame —
        // a hardware limit, see supportsPinExpander). The '595's width (8) is the chip's, not a setting,
        // so the toggle is a plain checkbox; latchPin is bound always (a saved value survives a
        // round-trip through direct mode) but shown only when the expander is on.
        controls_.addControl("pinExpander", pinExpander);
        controls_.setHidden(controls_.count() - 1, !(peripheral_ && peripheral_->supportsPinExpander()));
        controls_.addPin("latchPin", latchPin);
        controls_.setHidden(controls_.count() - 1, !pinExpanderMode());
        // The ring cluster (MoonI80 only; default no-op) — shift-mode geometry, all gated on the
        // expander being on, so it sits UNDER the pinExpander switch that governs it.
        if (peripheral_) peripheral_->addRingControls(controls_);
        // The on-device loopback self-test + its wiring — a dev/bring-up instrument (jumper a lane to the
        // rx pin), not a casual-user setting, so the whole cluster is expert-only.
        controls_.addControl("loopbackTest", loopbackTest);
        controls_.setAdvanced(controls_.count() - 1);
        // Always bound, shown only in test mode — the conditional-control shape.
        controls_.addPin("loopbackTxPin", loopbackTxPin);
        // Direct mode only. In shift mode the captured strand is decided by which '595 OUTPUT the
        // jumper is on (loopbackStrand), not by which GPIO transmits — runLoopbackSelfTest ignores
        // the override there — so showing it would only invite a mis-set control.
        controls_.setHidden(controls_.count() - 1, !loopbackTest || pinExpanderMode());
        controls_.setAdvanced(controls_.count() - 1);
        controls_.addPin("loopbackRxPin", loopbackRxPin);
        controls_.setHidden(controls_.count() - 1, !loopbackTest);
        controls_.setAdvanced(controls_.count() - 1);
        // Which strand carries the pattern — shift mode only (in direct mode it is always lane 0).
        // Lets the jumper come off ANY '595 output, including a spare one that drives no panel.
        controls_.addControl("loopbackStrand", loopbackStrand, 0, kMaxStrands - 1);
        controls_.setHidden(controls_.count() - 1, !loopbackTest || !pinExpanderMode());
        controls_.setAdvanced(controls_.count() - 1);
        // Intrusive: ride the live pipeline instead of a private replica (see the member doc).
        controls_.addControl("loopbackIntrusive", loopbackIntrusive);
        controls_.setHidden(controls_.count() - 1, !loopbackTest);
        controls_.setAdvanced(controls_.count() - 1);
    }

    /// A change to the pins, per-lane counts, the window, a derived bus control (clockPin/dcPin on
    /// i80), or `doubleBuffer` re-parses and re-inits the bus live via the prepare sweep.
    /// doubleBuffer is a prepare trigger because it drives ALLOCATION: OFF allocates one DMA buffer,
    /// ON (the default) allocates a second for the double-buffer — so toggling it must rebuild the bus
    /// to add or free that buffer (a board that turns it off pays no second-buffer memory).
    /// pinExpander / latchPin are prepare triggers too: the expander multiplies frameBytes_ ×8 and
    /// re-maps lanes onto pins, and the latch claims a bus bit — all of which is decided at bus init.
    bool affectsPrepare(const char* name) const override {
        return std::strcmp(name, "pins") == 0 || std::strcmp(name, "ledsPerPin") == 0
            || std::strcmp(name, "doubleBuffer") == 0
            || std::strcmp(name, "pinExpander") == 0 || std::strcmp(name, "latchPin") == 0
            || isWindowControl(name)
            || (peripheral_ && peripheral_->busControlTriggersBuild(name));   // clockPin/dcPin on i80
    }

    /// React to a control change off the render loop. loopbackTest is a persistent
    /// on/off mode: while ON, the self-test re-runs on every relevant change (with a
    /// lane-config refresh first, since onControlChanged precedes the prepare sweep);
    /// turning it OFF clears the verdict and re-derives the real driver status.
    void onControlChanged(const char* name) override {
        // Peripheral swap: bring the newly-selected backend's bus up. Scheduler::setControl already ran
        // rebuildControls() BEFORE this, and defineDriverControls calls ensurePeripheralMatchesSelection()
        // off peripheralSel_ (the just-written index) — so the live backend was ALREADY swapped during the
        // rebuild and the control list is bound to it. We must NOT swap again here: a second swapPeripheral
        // would free that backend and build a third, leaving every backend-owned control (clockPin, ring
        // cluster, ...) dangling into freed memory. ensurePeripheralMatchesSelection() is a no-op now
        // (peripheralActiveReg_ already equals the wanted reg); we only re-parse against the new backend's
        // descriptors (powerOfTwoBus differs Parlio vs i80) and reinit to bring the bus up.
        if (std::strcmp(name, "peripheral") == 0) {
            ensurePeripheralMatchesSelection();
            parseConfig();
            reinit();
            DriverBase::onControlChanged(name);
            return;
        }
        const bool isTestControl = std::strcmp(name, "loopbackTest") == 0;
        // Every control that reshapes the lane config the self-test transmits through. pinExpander
        // and latchPin belong here for the same reason `pins` does: they change laneList_,
        // frameBytes_ and latchBit_, so a test left running across such an edit would otherwise
        // re-transmit through a stale bus and report a verdict for a configuration that is gone.
        const bool isPinControl  = std::strcmp(name, "pins") == 0
                                || std::strcmp(name, "pinExpander") == 0
                                || std::strcmp(name, "latchPin") == 0
                                || std::strcmp(name, "loopbackTxPin") == 0
                                || std::strcmp(name, "loopbackRxPin") == 0;
        if (isTestControl && !loopbackTest) {
            // Toggling the test off clears the loopback's own verdict, then
            // re-derives the real driver status — a config/init error must
            // survive, which a blind clearStatus() would hide.
            clearFailBuf();
            clearStatus();
            parseConfig();
            reinit();
        } else if (loopbackTest && (isTestControl || isPinControl || isTestParamControl(name))) {
            // A pin edit changes laneList_/laneCount_/frameBytes_, but onControlChanged runs
            // BEFORE the prepare() sweep (and loopbackRxPin doesn't trigger that
            // sweep at all), so refresh the lane config here before testing it —
            // otherwise the self-test would build its private bus from stale pins.
            if (isPinControl) { parseConfig(); reinit(); }
            runLoopbackSelfTest();
        }
        // Chain to the base so a correction-control edit (localBrightness / preset / whiteMode)
        // rebuilds this driver's correction LUT — without this the LED driver's brightness/preset
        // controls were dead (only the global-brightness push reached the LUT).
        DriverBase::onControlChanged(name);
    }

    /// Loopback PARAMETER controls: not pins (no bus rebuild), but a change must re-run a running test so
    /// the verdict tracks the new setting (e.g. walk loopbackStrand to find the jumper, flip intrusive).
    static bool isTestParamControl(const char* name) {
        return std::strcmp(name, "loopbackStrand") == 0
            || std::strcmp(name, "loopbackIntrusive") == 0;
    }

    /// One-time wiring only (parse the lane lists into members); the bus acquire lives in
    /// prepare(), the sole resource gate. Enabled-independent — the acquire happens in the
    /// prepareTree sweep that always follows.
    void setup() override { parseConfig(); }
    /// Deinit the bus, then clear the shared fail/config-error state
    /// (DriverBase::release()).
    void release() override {
        deinit();   // drains in-flight first, so the ring's refill has stopped reading the snapshot
        freeSnapshot();
        DriverBase::release();   // frees the correction scratch, clears failBuf_ + configErr_
    }

    /// Free the ring snapshot buffer + clear the encode source and refresh the memory readout. Shared by
    /// release() and the ringSnapshot-off rebuild (the ring then reads the live source, so it isn't needed).
    /// The caller has already drained any in-flight refill (deinit / the rebuild's drain), so no ISR reads it.
    void freeSnapshot() {
        if (snapshotBuf_) { platform::free(snapshotBuf_); snapshotBuf_ = nullptr; snapshotCap_ = 0; this->publishHeapBytes(); }
        encodeSrc_ = nullptr;
    }

    /// Pure build (see MoonModule::prepare): re-parse the lanes and (re)init the bus off the
    /// hot path. No enabled() check — core's applyState() calls this only when effectively-enabled
    /// and routes to release() (bus + DMA buffer freed) otherwise, so a shared GPIO is released
    /// when the driver, or a parent, is disabled.
    void prepare() override {
        // Drain any in-flight transfer BEFORE parseConfig, not just before reinit's rebuild. parseConfig
        // reallocates the per-row scratch (prepareWire → ensureWire frees-then-allocs on a channel-count
        // change) and zeroes laneCounts_ — state the streaming ring's REFILL TASK reads concurrently on
        // its own core for the ~5 ms wire window. Draining first guarantees that reader has stopped, so a
        // grid resize or an RGB→RGBW switch mid-wire can't free the block the refill task is encoding
        // into. A no-op when idle, and correct for the whole-frame paths too (they had no second reader,
        // so it never mattered there — the ring is the first path that needs it).
        drainInFlight();
        parseConfig();
        reinit();
    }

    /// RGB<->RGBW changes the bytes-per-light and therefore the frame size, so re-parse and re-init the
    /// bus — but ONLY when the frame size actually changed. Skipped while (effectively) disabled (would
    /// re-grab the bus). Drains in-flight first (before parseConfig reallocates the scratch the ring's
    /// refill task reads) — see prepare().
    ///
    /// **The frame-size gate is what stops a redundant double-reinit.** The Drivers container's prepare()
    /// pushes every child's correction (passBufferToDrivers → rebuildCorrection → this) and THEN the same
    /// prepareTree sweep runs each child's own prepare() (which reinits unconditionally). Without the gate,
    /// a plain `ledsPerPin`/window edit — which changes the LIGHT COUNT but not the per-light CHANNELS, so
    /// frameBytes_ is unchanged by the correction push — would reinit here AND again in prepare(), tearing
    /// the bus down and rebuilding it twice in rapid succession. On the shift-mode LCD_CAM ring that second
    /// rapid rebuild came up with its GDMA EOF interrupt dead (the "output stalled" wedge). Gating on a real
    /// frameBytes_ change makes this a no-op in that path (only prepare() rebuilds), while a genuine RGB↔RGBW
    /// channel-count change — which prepare() does NOT necessarily follow (a standalone preset/whiteMode edit
    /// doesn't trigger prepareTree) — still rebuilds here.
    void onCorrectionChanged() override {
        if (!effectivelyEnabled()) return;
        const size_t before = frameBytes_;
        drainInFlight();
        parseConfig();
        if (frameBytes_ != before) reinit();   // rebuild only on a real frame-size change (see above)
    }

    /// Point the driver at the source frame buffer and re-parse the lane config.
    void setSourceBuffer(Buffer* buf) override { sourceBuffer_ = buf; parseConfig(); }

    /// Per-tick output (deferred-wait double-buffer): a fused per-ROW pass corrects the same
    /// light index of every active lane and transposes it into 3-slot bus words in the DMA buffer,
    /// then ships it as one autonomous transfer. Two paths, chosen by whether a second DMA buffer is
    /// allocated (which `doubleBuffer` drives at init): SYNCHRONOUS (default, one buffer — the
    /// original encode→transmit→wait, 0 added latency, provably no regression) or DEFERRED-WAIT
    /// DOUBLE-BUFFER (doubleBuffer ON — encode N+1 while N clocks out, `max(encode, wire)` per tick,
    /// +1 frame latency, +1 DMA buffer). Inert off this chip and idle until inited with a source
    /// buffer + correction. (The double-buffer defaults ON — it overlaps the blocking wire wait and
    /// lifted the P4 whole-board rate 48→76 fps; OFF is the audio-reactive 0-latency opt-out and pays
    /// for exactly one buffer — see the doubleBuffer control + docs/history/lessons.md.)
    // REPORTED AS BLOCKING, deliberately: tickSync()/tickRing() reach busWaitIfBusy(), which
    // waits for the DMA transfer to finish (bounded by waitBudgetMs, and self-limiting via
    // deadFrames_). The wait is by design — the driver owns the bus for the frame, and encoding
    // over a live transfer corrupts output — but it IS blocking, so clang-hotpath lists it.
    // Not suppressed: a report of what blocks the render path is worthless if the two worst
    // offenders are hidden from it. Moving the wait off the render path is backlogged
    // (backlog-core: hot path).
    void tick() MM_NONBLOCKING override {
        if (!peripheral_ || peripheral_->lanesAvailable() == 0) return;  // no backend / inert off this chip
        // Loopback mode owns the peripheral EXCLUSIVELY. While it is on, the render loop must not
        // transmit — the loopback tears the bus down, drives its own private frame, and rebuilds it.
        if (loopbackTest) return;
        if (!inited_ || !dmaBuf_ || !sourceBuffer_ || !sourceBuffer_->data()
            || laneCount_ == 0 || maxLaneLights_ == 0) return;
        const uint8_t outCh = correction_.outChannels;
        if (outCh == 0) return;
        // The per-row scratch must be allocated and sized for this channel count (prepareWire, off the
        // hot path). If it isn't (alloc failed / a shrunk realloc pending), idle rather than overrun.
        if (!wire_ || wireCap_ < static_cast<size_t>(kMaxStrands) * outCh * platform::kMaxCores) return;

        // Three explicitly-separate paths. RING (MoonI80's oversize-shift path) streams the frame from
        // small internal buffers refilled by the platform, so it does NOT hold the whole frame and the
        // busCapacity() guard below (which the other two need) does not apply. The whole-frame paths keep
        // their guard and their proven behavior byte-for-byte.
        if (peripheral_->busIsRing()) { tickRing(outCh); return; }
        // A frame past what the peripheral's single transfer can carry (Parlio: 65535 bytes) used to
        // return here silently: the LEDs froze on their last frame, the render loop and UI stayed
        // healthy, and only a reboot appeared to help — until the count crossed the line again. Say
        // it instead, with the number the user has to act on: the ceiling in LIGHTS PER LANE, since
        // that is the control they set. (github.com/MoonModules/projectMM/issues/44)
        if (frameBytes_ > peripheral_->busCapacity()) {
            reportOverCapacity(outCh, peripheral_->busCapacity());
            return;
        }

        // Two explicitly-separate whole-frame paths so the OFF path is PROVABLY the pre-Step-1.5 behavior
        // (no regression) and pays nothing for the double-buffer it isn't using. The mode is fixed by
        // whether the second buffer was allocated (busInit(async) — ON allocs it, OFF doesn't), so a
        // stale flag can't route a single-buffer bus down the async path.
        if (peripheral_->busBuffer(1)) tickAsync(outCh);   // double-buffer (doubleBuffer ON)
        else                           tickSync(outCh);    // synchronous (doubleBuffer OFF / no 2nd buf)
    }

    // Synchronous single-buffer path — the ORIGINAL tick, verbatim: encode buffer 0, transmit, wait
    // right here. One DMA buffer, no alternation, no deferred-wait bookkeeping, 0 added latency. This
    // is the default (doubleBuffer OFF) and its timing is exactly the pre-double-buffer driver's.
    /// Blocking path (doubleBuffer OFF): encode the frame, send it, and wait out the wire before
    /// returning — so a tick costs encode + wire. One DMA buffer, no output latency.
    void tickSync(uint8_t outCh) {
        if (busGaveUp()) return;
        // A previous frame's wait may have timed out, leaving the DMA still reading buffer 0 — re-wait
        // rather than encoding over a live transfer. (Normally a no-op: the wait below completes.)
        if (!busWaitIfBusy(0)) return;
        uint8_t* buf = peripheral_->busBuffer(0);
        if (!buf) return;
        // Branch on the BUS WIDTH (slotBytes), not the strand count — with a '595 expander the
        // strands ride the shift cycles, so 48 strands on 6 pins is still an 8-bit bus.
        if (slotBytes() == 1) encodeRows<uint8_t>(outCh, buf);
        else                  encodeRows<uint16_t>(outCh, buf);
        // The latch pad (zeroed at reinit, never written here) ends the transfer holding every lane
        // LOW >=300 µs. Wait only when the transfer started — a failed transmit gives no done-callback,
        // so an unconditional wait would block the full timeout. A timed-out wait leaves the buffer
        // marked in-flight so the next tick re-waits instead of corrupting the live transfer.
        if (peripheral_->busTransmit(0, frameBytes_)) {
            inFlight_[0] = true;
            busWaitIfBusy(0);   // synchronous: wait it out here; clears the flag on completion
        } else if (deadFrames_ < kDeadFramesBeforeGiveUp) {
            deadFrames_++;      // a REFUSED frame is as dead as a wedged one — see busWaitIfBusy
        }
    }

    // Deferred-wait double-buffer path (doubleBuffer ON) — encode frame N+1 into the back buffer
    // while frame N clocks out of the front, so the per-tick wall-clock is max(encode, wire) instead
    // of encode + wire. Costs the second DMA buffer + 1 frame of output latency. See the class doc.
    /// Deferred-wait path (doubleBuffer ON, the default): encode frame N+1 into the back buffer while
    /// frame N clocks out of the front, so a tick costs max(encode, wire) instead of encode + wire. Costs
    /// a second DMA buffer and one frame of output latency.
    void tickAsync(uint8_t outCh) {
        if (busGaveUp()) return;
        // 1. Finish the transfer that last used the buffer we're about to encode into, so the encode
        //    never overwrites a frame still clocking out (no-op on the first tick). If that wait TIMES
        //    OUT the DMA is still reading the buffer — skip this frame entirely rather than encoding
        //    into a live transfer (which would corrupt the frame on the wire). The buffer stays marked
        //    in-flight, so the next tick re-waits; a genuinely wedged DMA therefore idles the driver
        //    instead of emitting garbage, and it self-heals the moment the transfer completes.
        if (!busWaitIfBusy(active_)) return;
        // 2. Fused per-ROW encode into buffer `active_`, one branch on the bus width (see encodeRows).
        uint8_t* buf = peripheral_->busBuffer(active_);
        if (!buf) return;
        // Branch on the BUS WIDTH (slotBytes), not the strand count — with a '595 expander the
        // strands ride the shift cycles, so 48 strands on 6 pins is still an 8-bit bus.
        if (slotBytes() == 1) encodeRows<uint8_t>(outCh, buf);
        else                  encodeRows<uint16_t>(outCh, buf);
        // 3. Kick this buffer's DMA and return WITHOUT waiting; flip to the other buffer for next tick.
        if (peripheral_->busTransmit(active_, frameBytes_)) {
            inFlight_[active_] = true;
            active_ ^= 1;
        } else if (deadFrames_ < kDeadFramesBeforeGiveUp) {
            deadFrames_++;      // a REFUSED frame is as dead as a wedged one — see busWaitIfBusy
        }
    }

    // Streaming-ring path (MoonI80, oversize shift mode) — the frame is too big to hold, so the platform
    // streams it from a few small internal buffers, refilled by a pinned task (ringEncodeTrampoline →
    // encodeRows) as the DMA drains them.
    //
    // **ASYNC per tick, like tickAsync — the render thread must NOT block on the wire.** The refill task
    // has a hard real-time deadline (the DMA drains one buffer in ~360 µs, so a slice must be re-encoded
    // within that), and it runs on the same core as the render loop. If tickRing BLOCKED here waiting out
    // the whole ~6 ms wire, the render thread would hold the core across that window — and any work that
    // piled on (a `/api/state` serialize from a UI refresh, a WiFi burst) would starve the refill task
    // past its deadline, the DMA would hit an un-refilled buffer, and the frame would stall (the
    // "freezes when I refresh the UI" bug). So: WAIT for the previous frame (a no-op if it finished while
    // the render ran), START the next, and RETURN — the wire and its refills proceed in the background
    // with the core free. One frame of latency, exactly as the double-buffer trades.
    /// Streaming-ring path: wait for the previous frame, arm the next, and RETURN — the wire and its
    /// refills run in the background. Never blocks the render core for a whole frame, which is what keeps
    /// the UI responsive at sizes where the wire takes tens of milliseconds.
    void tickRing(uint8_t /*outCh*/) {
        if (busGaveUp()) return;
        // BENCH DIAGNOSTIC: attribute the ring tick's three segments — wire-wait / snapshot / prime —
        // read out via ringDbg (tw/ts/tp µs). Scope: the fps work in the backlog's ring entry.
        const uint32_t tkW0 = platform::cycleCount();
        // Finish the PREVIOUS ring frame before starting the next (a strand receives one frame at a time;
        // the ring reports completion on slot 0). Normally already done — the whole render tick elapsed
        // since we kicked it. A timed-out wait leaves the frame in-flight so the next tick re-waits rather
        // than starting a second frame over a live one.
        if (!busWaitIfBusy(0)) return;
        const uint32_t tkW1 = platform::cycleCount();
        // Freeze the source for this frame BEFORE kicking the transfer: busTransmitRing primes the first
        // ring buffers synchronously (trampoline → encodeRows) and its refill re-encodes the rest off the
        // render thread across the ~6 ms wire, so both must read an immutable copy, not the live Layer
        // buffer the render loop is free to overwrite. A failed snapshot (source gone / OOM) skips the
        // frame rather than encoding from a moving source. The `ringSnapshot` A/B knob turns this off to
        // read the live source directly (matches pre-snapshot behavior) — a bench measurement lever, not a
        // shipping opt-out; it defaults ON.
        if (ringSnapshot) { if (!snapshotSourceForRing()) return; }
        else              encodeSrc_ = nullptr;   // OFF: encodeRows reads the live sourceBuffer_
        const uint32_t tkW2 = platform::cycleCount();
        if (peripheral_->busTransmitRing()) {
            inFlight_[0] = true;   // kicked; DO NOT wait here — the next tick waits, freeing the core now
        } else if (deadFrames_ < kDeadFramesBeforeGiveUp) {
            deadFrames_++;         // a REFUSED frame is as dead as a wedged one — see busWaitIfBusy
        }
        const uint32_t tkW3 = platform::cycleCount();
        constexpr uint32_t kCyPerUs = 240;   // S3 at 240 MHz
        dbgTickWaitUs = (tkW1 - tkW0) / kCyPerUs;
        dbgTickSnapUs = (tkW2 - tkW1) / kCyPerUs;
        dbgTickPrimeUs = (tkW3 - tkW2) / kCyPerUs;
    }

    // BENCH DIAGNOSTIC: the ring tick's three segment costs (µs), surfaced in ringDbg as tw/ts/tp.
    // Static (one hot ring driver on the bench); scope: the fps work in the backlog's ring entry.
    static inline volatile uint32_t dbgTickWaitUs = 0;
    static inline volatile uint32_t dbgTickSnapUs = 0;
    static inline volatile uint32_t dbgTickPrimeUs = 0;

    /// Refresh the read-only `frameTime` KPI once a second (off the hot path): the last measured DMA
    /// wire time and the fps ceiling it implies (1e6 / frameTime). This is the pure WS2812 output floor —
    /// the render loop can never beat it, so it's the target the multicore work drives the system tick
    /// toward, and it tracks an overclocked slot rate directly. "—" until the first transfer completes.
    void tick1s() MM_NONBLOCKING override {
        if (!peripheral_) return;
        // A bus that lost a shared peripheral to another module comes back on its own once that
        // module lets go. On the classic ESP32 the LED bus is an I2S peripheral and drives from
        // instance 1, so whichever asks second is refused; without this the loser stayed dark until the user
        // happened to edit a control, which is a reboot-to-apply in all but name (architecture.md,
        // live reconfiguration). Gated tightly, because this runs on the render thread: only while
        // the driver WANTS the bus and does not hold it, and only when the backend says the thing
        // it was refused is free again (a register read, not an init). The rebuild itself is the
        // same reinit() a control edit runs, on the same cold path, at most once per second.
        if (!inited_ && laneCount_ > 0 && peripheral_->busContentionCleared()) reinit();
        const uint32_t us = peripheral_->busLastTransmitUs();
        if (us == 0) std::snprintf(frameTimeStr_, sizeof(frameTimeStr_), "—");
        else std::snprintf(frameTimeStr_, sizeof(frameTimeStr_), "%u µs (%u fps max)",
                           static_cast<unsigned>(us), static_cast<unsigned>(1000000u / us));
        peripheral_->refreshBusKpi();   // per-backend extra read-only KPIs (MoonI80's ring diagnostic); base no-op
    }

    // Wait for buffer `i`'s in-flight transfer to finish. Returns TRUE when the buffer is free to
    // reuse — either nothing was outstanding (first tick, or a transmit that failed to start, so an
    // unconditional wait can't block the full timeout) or the transfer completed. Returns FALSE when
    // the wait TIMED OUT: the DMA may still be reading, so `inFlight_` stays set and the caller must
    // not touch the buffer. Used by tickAsync (the deferred-wait double-buffer path) on the buffer it
    // is about to REUSE; the synchronous path (tickSync) waits inline and never uses this.
    /// Wait for buffer `i`'s transfer if one is still in flight; false means it wedged (the caller then
    /// skips the frame rather than writing into memory the DMA may still be reading). A no-op when idle,
    /// so callers arm it unconditionally instead of tracking state themselves.
    bool busWaitIfBusy(uint8_t i) {
        if (!inFlight_[i]) return true;
        if (!peripheral_ || !peripheral_->busWait(i, waitBudgetMs())) {
            // The transfer did not complete within many times its own wire time, so it is not going
            // to. Count it: a bus that keeps doing this is broken, and the ONLY thing that matters
            // then is that the driver stops spending the render thread on it (see deadFrames_).
            // A transfer the peripheral REFUSES outright earns the same strike, counted at the
            // busTransmit call sites — it never reaches here, because nothing went in flight.
            if (deadFrames_ < kDeadFramesBeforeGiveUp) deadFrames_++;
            return false;   // still in flight — do not reuse the buffer
        }
        deadFrames_ = 0;   // a completed transfer clears the strike count: the bus is alive again
        inFlight_[i] = false;
        // If we'd already reported give-up ("output stalled"), a completed transfer means the bus
        // recovered — re-derive the real status so the UI stops showing a fault the device no longer has.
        // parseConfig() (not a bare clearStatus) is what RESTORES the normal "driving N of M lights" info;
        // clearStatus alone would wipe the error but leave the status blank forever (the info is only set
        // in parseConfig, which the retry path doesn't otherwise re-run).
        if (gaveUpReported_) { gaveUpReported_ = false; parseConfig(); }
        return true;
    }

    /// How long a transfer gets before we call it dead. **Derived from the frame, never a constant.**
    /// The wire time is `frameBytes / pclk` by construction, so a healthy transfer completes in a few
    /// ms; a fixed 1 s timeout (what this used) is 20× a whole tick budget, so a single undelivered
    /// frame would block the render thread for 20 ticks' worth of CPU — enough to starve WiFi off the
    /// air. That is how a merely *misconfigured* driver made a device unreachable (bench, 2026-07-14),
    /// which is a robustness bug in its own right: no LED setting may cost the user the device.
    ///
    /// **A broken bus must not cost the user the device.** Every stalled frame is a wait on the RENDER
    /// thread, so a bus that never delivers doesn't merely fail to light LEDs — it eats the CPU that
    /// WiFi, HTTP and the UI need, and the device drops off the network with no way back in. That is
    /// how a *misconfigured LED driver* (a frame the DMA can't sustain from PSRAM) made a bench board
    /// unreachable and unrecoverable-without-a-cable, 2026-07-14.
    ///
    /// So after kDeadFramesBeforeGiveUp consecutive dead transfers, stop transmitting: report the failure
    /// and let the tick return immediately. The LEDs go dark — but the device stays *reachable*, so the
    /// user can see the status and fix the setting that caused it. Degraded, not crashed; the
    /// *Robustness* rule ([architecture.md](../../../docs/architecture.md#robustness)) says a bad input
    /// may leave the output idle, never the device wedged.
    ///
    /// It self-heals: any completed transfer clears the strike count, and a config change re-inits the
    /// bus (prepare → reinit → deadFrames_ = 0), so fixing the setting brings the LEDs straight back with
    /// no reboot.
    ///
    /// **Give-up is not permanent — it periodically RETRIES.** A *misconfigured* bus never delivers and
    /// stays given-up (correct: stop spending the render thread). But a *transient* stall — the streaming
    /// ring's refill missing one deadline because a `/api/state` serialise stole the core for a few ms —
    /// is recoverable: the next frame would succeed. Latching give-up until a reinit turns a momentary
    /// hiccup into "LEDs dark until you touch a control or reboot," which is itself a robustness failure.
    /// So once given up, let ONE frame through every `kGiveUpRetryTicks` ticks: if it completes, the
    /// strike count clears and output resumes on its own; if it doesn't, we fall straight back to
    /// given-up, having spent one frame's wait per ~`kGiveUpRetryTicks` ticks — cheap enough not to
    /// starve the network, unlike retrying every tick.
    /// The frame does not fit one transfer. Report the ceiling the way the user sets it — lights per
    /// lane — rather than the byte figure they would have to derive it from. Cleared by reinit(), so
    /// lowering the count restores normal reporting.
    /// `cap` is the byte ceiling to measure against — the peripheral's live buffer capacity on the
    /// tick path, or its declared DMA budget at reinit(), where no bus exists yet and busCapacity()
    /// would read 0. Passing it in keeps one message for both, instead of a KB figure on one path
    /// and the actionable light count on the other.
    void reportOverCapacity(uint8_t outCh, size_t cap) {
        if (overCapReported_) return;
        overCapReported_ = true;
        const uint8_t opp     = outputsPerPin();
        const size_t  rowBytes = rowBytesFor(outCh, slotBytes(), opp);
        const size_t  pad     = padBytesFor(slotBytes(), opp);
        // Count DOWN through frameBytesFor, not up through a division: the frame is 64-byte ROUNDED,
        // so `(cap - pad) / rowBytes` overshoots by one — it reported 898 lights, whose frame rounds
        // to 65536 against a 65535 cap. A limit that still fails is worse than no limit.
        unsigned fits = 0;
        if (rowBytes && cap > pad) {
            fits = static_cast<unsigned>((cap - pad) / rowBytes);
            while (fits > 0 && frameBytesFor(static_cast<nrOfLightsType>(fits), outCh,
                                             slotBytes(), opp) > cap) fits--;
        }
        std::snprintf(statusBuf_, sizeof(statusBuf_),
                      "too many lights per pin: %u exceeds this peripheral's %u — lower ledsPerPin",
                      static_cast<unsigned>(maxLaneLights_), fits);
        setStatus(statusBuf_, Severity::Error);
    }

    bool busGaveUp() {
        if (deadFrames_ < kDeadFramesBeforeGiveUp) return false;
        if (!gaveUpReported_) {
            gaveUpReported_ = true;
            setStatus("no LED output — the driver is not sending frames; check pins and LED count",
                      Severity::Error);
        }
        // Periodic retry window: every kGiveUpRetryTicks-th call, return false so the caller attempts one
        // transfer. A completed transfer clears deadFrames_ (busWaitIfBusy) and lifts the give-up; a
        // failed one re-increments and we stay given-up until the next window.
        if (++giveUpRetry_ >= kGiveUpRetryTicks) {
            giveUpRetry_ = 0;
            deadFrames_ = kDeadFramesBeforeGiveUp - 1;   // one strike below the threshold: let this frame try
            // ABANDON the wedged in-flight transfer so the retry starts CLEAN. After this many timeouts
            // the transfer is not going to complete; leaving inFlight_ set would make the tick's
            // busWaitIfBusy() wait on that dead transfer (and time out again) instead of arming a fresh
            // one. Clearing the flags lets the next tick encode+transmit a replacement — the transmit
            // re-arms the bus, which is what actually recovers it.
            inFlight_[0] = inFlight_[1] = false;
            // Do NOT clear the "output stalled" status here — the retry has not SUCCEEDED yet, it is only
            // about to try. Leave gaveUpReported_ set so the status stays honest during the attempt: if the
            // retry frame completes, busWaitIfBusy() re-derives the real "driving N of M" status (via
            // parseConfig); if it fails again, the error was correctly still showing the whole time. This
            // is the fix for the earlier bug where an eager clear wiped the status to blank forever (the
            // driving-info is only re-set on a successful transfer, not on the retry attempt itself).
            return false;
        }
        return true;
    }

    /// Computed from the WS2812 wire contract, which is the same on every backend and needs no
    /// platform constant: one light clocks `channels × 8` bits, each bit is 3 slots, and a slot is
    /// ~375 ns of WIRE time — the expander changes how many bus words fill a slot, not how long the
    /// strand takes. So `maxLaneLights_` (the longest strand) sets the frame's wire time directly.
    ///
    /// 4× that, floored at 20 ms (a tiny frame still tolerates scheduling jitter) and capped at
    /// 100 ms (even a huge frame cannot blow a tick budget). Generous enough never to kill a healthy
    /// transfer; small enough that a dead one costs a frame, not a second.
    uint32_t waitBudgetMs() const {
        constexpr uint32_t kSlotNs = 375;   // WS2812 wire slot; 3 per bit (ParallelSlots.h)
        const uint32_t bits = static_cast<uint32_t>(maxLaneLights_) * correction_.outChannels * 8u;
        const uint32_t wireMs = (bits * 3u * kSlotNs) / 1'000'000u;
        const uint32_t budget = wireMs * 4u;
        return budget < 20u ? 20u : (budget > 100u ? 100u : budget);
    }

    // Drain BOTH buffers' in-flight transfers before a reinit/release frees them — a live DMA reading
    // a buffer that's about to be freed is a use-after-free. Safe to call any time (busWaitIfBusy is a
    // no-op on an idle buffer). If a wait times out here we cannot simply skip: the buffers are about
    // to go away, so the peripheral itself is torn down (deinit stops the unit / deletes the bus,
    // which cancels any transfer) — the flag is cleared so the teardown proceeds rather than spinning.
    /// Block until nothing is reading the DMA buffers or the source snapshot — the barrier that makes a
    /// live resize safe (a grid change or an RGB->RGBW switch must not free memory a transfer is reading).
    void drainInFlight() {
        if (!busWaitIfBusy(0)) inFlight_[0] = false;   // deinit below cancels the wedged transfer
        if (!busWaitIfBusy(1)) inFlight_[1] = false;
    }

    /// Prefill both DMA buffers' shift-mode constants (a no-op in direct mode, where every slot word
    /// already carries data or is a zero the buffer's memset provides). Called from reinit(), the cold
    /// path, whenever the buffers are (re)established or re-zeroed.
    void prefillShiftConstantsIfNeeded() {
        if (!peripheral_ || !pinExpanderMode() || !inited_) return;
        const uint8_t outCh = correction_.outChannels;
        if (outCh == 0 || maxLaneLights_ == 0) return;
        for (uint8_t i = 0; i < 2; i++) {
            uint8_t* buf = peripheral_->busBuffer(i);
            if (!buf) continue;   // buffer 1 is null in single-buffer mode
            if (slotBytes() == 1) prefillShiftFrame<uint8_t>(outCh, buf);
            else                  prefillShiftFrame<uint16_t>(outCh, buf);
        }
    }

    /// Write the shift-mode frame CONSTANTS into every DMA buffer, once, off the hot path.
    ///
    /// Of the three words a WS2812 slot emits per shift cycle, two are the same for every light: the
    /// pulse-start (which strands are live) and the pulse-tail (all-LOW). Only the middle word carries
    /// pixel data. Pre-filling the constants here means `encodeRows` writes **one word instead of
    /// three** — two thirds of the encoder's stores, gone from the per-frame path. (Measured on an S3:
    /// ~9.7 µs/light → ~3 µs.) hpwit does the same with `putdefaultones()` at buffer init; studied,
    /// then written fresh against our own layout.
    ///
    /// **Short strands make this row-dependent, not frame-uniform.** A strand that runs out mid-frame
    /// drops out of the active set at that row, and its pulse-start word must stop asserting from
    /// there on — otherwise the exhausted strand flashes white at full brightness (the bug this
    /// morning). So the frame is prefilled in RUNS: rows sharing an active mask are one call, and a
    /// new run starts wherever a strand ends. Equal-length strands — the common case — are a single run.
    template <class Slot>
    void prefillShiftFrame(uint8_t outCh, uint8_t* dst) {
        prefillShiftRows<Slot>(outCh, dst, 0, maxLaneLights_);
    }

    /// Prefill the shift constants for rows [firstRow, firstRow + rowCount) into `dst`, written
    /// dst-RELATIVE (row `firstRow` lands at dst+0) — so a ring buffer holding one slice gets its
    /// constants laid down exactly as the whole-frame buffer does for the same rows. `rowCount == 0`
    /// means "to the end". The whole-frame path calls this as [0, all); the ring calls it per slice
    /// (which is why the constants land on a recycled ring buffer that encodeRows alone would leave
    /// stale — encodeRows writes only the DATA word, relying on these constants being present).
    template <class Slot>
    void MM_RAMFUNC prefillShiftRows(uint8_t outCh, uint8_t* dst, nrOfLightsType firstRow, nrOfLightsType rowCount) {
        auto* out = reinterpret_cast<Slot*>(dst);
        const size_t slotsPerRow = static_cast<size_t>(outCh) * 8 * 3 * outputsPerPin();
        const nrOfLightsType lastRow = (rowCount == 0 || firstRow + rowCount > maxLaneLights_)
                                           ? maxLaneLights_
                                           : static_cast<nrOfLightsType>(firstRow + rowCount);
        nrOfLightsType row = firstRow;
        while (row < lastRow) {
            // The active set for this row, and how many rows keep it (until the next strand ends).
            // 32-bit halves: `1ULL << lane` with a runtime lane is a library call on Xtensa (see
            // encodeWs2812ShiftData's maskLo/maskHi note); this runs per refill on ragged configs.
            uint32_t mLo = 0, mHi = 0;
            nrOfLightsType runEnd = lastRow;
            for (uint8_t lane = 0; lane < laneCount_; lane++) {
                if (row < laneCounts_[lane]) {
                    if (lane < 32) mLo |= uint32_t(1) << lane;
                    else           mHi |= uint32_t(1) << (lane - 32);
                    if (laneCounts_[lane] < runEnd) runEnd = laneCounts_[lane];   // this strand ends first
                }
            }
            const uint64_t mask = mLo | (static_cast<uint64_t>(mHi) << 32);   // constant shift: cheap
            if (runEnd <= row) runEnd = lastRow;   // no strand ends ahead in range: one run to lastRow
            const uint32_t rows = static_cast<uint32_t>(runEnd - row);
            // dst-relative: row `firstRow` is at out+0, so offset by (row - firstRow).
            prefillWs2812ShiftConstants<Slot>(mask, physPins_, latchBit_, outputsPerPin(), outCh, rows,
                                              out + static_cast<size_t>(row - firstRow) * slotsPerRow);
            row = runEnd;
        }
    }

    // Encode rows [firstRow, firstRow + rowCount) into `dst` as `Slot`-wide bus words (uint8_t for the
    // 8-bit bus, uint16_t for the 16-bit bus). Correct the same light index of every active lane into
    // the wire block, then transpose+emit its 3 slots. No heap, integer math only. `dst` is the raw-byte
    // DMA buffer this tick owns; a 16-bit frame views it as uint16 slots (sized ×2 by frameBytesFor).
    //
    // **A ROW RANGE, not always the whole frame** — the whole-frame call is just the range [0, all).
    // The streaming ring (MoonI80's phase-2 path) encodes one slice at a time straight into the small
    // internal buffer the DMA is about to read, so the big encoded frame is never materialised at all.
    // Slicing is why it can: the loop was already per-row, so a range costs nothing extra.
    //
    // `rowCount == 0` means "to the end". Only the LAST slice closes the frame with the latch pad, so a
    // partial slice must not emit it — hence `closeFrame`.
    /// Encode rows `[firstRow, firstRow + rowCount)` into `dst` — the whole frame in one call for the
    /// direct paths, one slice per call for the ring (`rowCount == 0` means "to the end"). Writes
    /// dst-relative, so a slice lands at dst+0 whatever its first row. `closeFrame` appends the latch
    /// pad, so only the LAST slice may set it.
    template <class Slot>
    void MM_RAMFUNC encodeRows(uint8_t outCh, uint8_t* dst,
                    nrOfLightsType firstRow = 0, nrOfLightsType rowCount = 0,
                    bool closeFrame = true) {
        const nrOfLightsType lastRow = (rowCount == 0 || firstRow + rowCount > maxLaneLights_)
                                           ? maxLaneLights_
                                           : static_cast<nrOfLightsType>(firstRow + rowCount);
        // encodeSrc_ is the IMMUTABLE per-frame source when the ring snapshotted (a plain memcpy of the
        // live window at srcCh stride — bias-corrected so the index is unchanged); null on the sync/async
        // whole-frame paths, which read the live buffer. EITHER WAY the correction runs HERE, per light,
        // during the gather — the snapshot is a raw copy, not pre-corrected. Fusing correction into this
        // one pass (instead of a separate correct-then-copy pass in the snapshot) is measurably cheaper:
        // the per-refill encode barely moves (~36 µs at 12K, lt=0 holds), and it deletes the whole
        // ~4.7 ms pre-correction pass — while the memcpy keeps the immutability the snapshot exists for.
        const uint8_t* src = encodeSrc_ ? encodeSrc_ : sourceBuffer_->data();
        const uint8_t srcCh = sourceBuffer_->channelsPerLight();
        auto* out = reinterpret_cast<Slot*>(dst);
        // Per-lane wire slots, lane-major with stride `outCh` (wire_[lane * outCh + ch]). Sized to the
        // channel count off the hot path (prepareWire), so a light of any channel count (RGB / RGBW /
        // RGBCCT / an N-channel fixture) is laid out without overrun. Zeroed each frame so a
        // short-strand lane's slot (skipped below) contributes no set bit (the activeMask rule already
        // gates it, but this removes the read-of-uninitialised footgun).
        // THIS CORE's scratch slice (per-CPU data): the prime fork-join runs encodeRows on both cores
        // at once, so each core gathers/transposes through its own slice — no locking, no sharing.
        uint8_t* const wire = wire_ + static_cast<size_t>(platform::currentCore()) * kMaxStrands * outCh;
        std::memset(wire, 0, static_cast<size_t>(kMaxStrands) * outCh);
        const size_t stride = outCh;
        const bool shift = pinExpanderMode();
        // BENCH DIAGNOSTIC: attribute the refill's cycles per segment — gather+mask vs the
        // transpose+emit — read out via ringDbg's sg/se fields (scope: the backlog's ring entry).
        uint32_t segT0 = platform::cycleCount();
        // The active-strand mask is 64-bit because a '595 expander drives more strands than the bus
        // is wide (up to kMaxStrands); in direct mode only the low `laneCount_` bits are ever set,
        // and it narrows to Slot for the direct encoder.
        for (nrOfLightsType row = firstRow; row < lastRow; row++) {
            // Built as 32-bit halves (combined once below): `1ULL << lane` with a runtime lane is a
            // library call on Xtensa — 48 of them per row was a measured chunk of the refill cost.
            uint32_t maskLo32 = 0, maskHi32 = 0;
            for (uint8_t lane = 0; lane < laneCount_; lane++) {
                if (row >= laneCounts_[lane]) continue;   // short strand: idle LOW
                if (lane < 32) maskLo32 |= uint32_t(1) << lane;
                else           maskHi32 |= uint32_t(1) << (lane - 32);
                // winStart_ shifts this driver's whole slice; laneStart_ is the per-lane offset within it.
                // The source (snapshot or live) holds RAW srcCh bytes, so correction runs here per light —
                // one pass, whether the frame was snapshotted (immutable copy) or read live.
                correction_.apply(src + (winStart_ + laneStart_[lane] + row) * srcCh,
                                  wire + lane * stride, srcCh);
            }
            const uint64_t mask = maskLo32 | (static_cast<uint64_t>(maskHi32) << 32);   // constant shift: cheap
            if (shift) {
                // DATA WORDS ONLY. The pulse-start and pulse-tail words of every slot are frame
                // constants and were written once by prefillShiftFrame() — two thirds of the stores,
                // hoisted straight out of the hot path (see prefillWs2812ShiftConstants).
                const uint32_t segT1 = platform::cycleCount();   // TEMP DIAGNOSTIC
                encodeWs2812ShiftData<Slot>(wire, mask, physPins_, latchBit_, outputsPerPin(), outCh, out);
                const uint32_t segT2 = platform::cycleCount();   // TEMP DIAGNOSTIC
                dbgSegGatherCy += segT1 - segT0;   // memset+mask+gather (since segT0 / previous row's end)
                dbgSegEmitCy   += segT2 - segT1;   // the transpose+emit
                dbgSegRows     += 1;
                segT0 = segT2;                     // next row's gather starts here
                out += static_cast<size_t>(outCh) * 8 * 3 * outputsPerPin();
            } else {
                encodeWs2812ParallelSlots<Slot>(wire, static_cast<Slot>(mask), outCh, out);
                out += static_cast<size_t>(outCh) * 8 * 3;   // 3 slots × 8 bits × channels, in Slot elements
            }
        }
        // Close the frame: one latch-only word at the head of the (zeroed) latch pad, so the final
        // byte is actually presented and every strand idles LOW through the pad — the WS2812 reset.
        // Without it a strand whose last wire byte is ODD holds HIGH for the whole pad and never
        // resets (see encodeWs2812ShiftLatchPad). Direct mode needs nothing: a zeroed pad IS a LOW line.
        if (shift && closeFrame) encodeWs2812ShiftLatchPad<Slot>(latchBit_, out);
    }

    /// Write ONLY the frame-closing latch word at `dst` — the shift expander's one-more-latch that
    /// presents the register's final slot on the strand (see encodeWs2812ShiftLatchPad). The ring's
    /// zero-lap head carries it via the seam's close call; direct mode needs nothing (a zeroed buffer
    /// is already a clean LOW), so this is a no-op there.
    void MM_RAMFUNC encodeFrameClose(uint8_t* dst) {
        if (!pinExpanderMode()) return;
        if (slotBytes() == 1) encodeWs2812ShiftLatchPad<uint8_t>(latchBit_, reinterpret_cast<uint8_t*>(dst));
        else                  encodeWs2812ShiftLatchPad<uint16_t>(latchBit_, reinterpret_cast<uint16_t*>(dst));
    }

    // Encode the loopback test frame at `Slot` width: the same pattern on lane 0 in every
    // row (activeMask = bit 0). Matches the private loopback bus's width so the DMA stride
    // is right. `frame` is raw bytes; viewed as Slot elements.
    /// Build the self-test frame for DIRECT mode: the known pattern on lane 0, every other lane idle.
    /// Encoded at the operational bus width so the DMA stride matches the real thing.
    template <class Slot>
    void encodeLoopbackFrame(uint8_t* frame, const uint8_t* wire, uint8_t outCh,
                             nrOfLightsType lights) {
        auto* out = reinterpret_cast<Slot*>(frame);
        for (nrOfLightsType row = 0; row < lights; row++) {
            encodeWs2812ParallelSlots<Slot>(wire, Slot(1), outCh, out);
            out += static_cast<size_t>(outCh) * 8 * 3;
        }
    }

    // The shift-register sibling: the same test pattern, but encoded through the '595 fan-out so the
    // frame the private bus transmits is byte-for-byte what the render loop sends in shift mode.
    // activeMask = bit 0 → STRAND 0, which is data pin 0's register at shift position 0. Because a
    // '595 shifts MSB-first, that strand appears on the register's LAST output (Q7) — that is the pin
    // the jumper must come from. The latch rides its own bus bit (latchBit_), as it does at runtime.
    /// Build the self-test frame for EXPANDER mode: the pattern on one '595 output (`loopbackStrand`),
    /// every other strand dark, the latch on its own bus bit exactly as at runtime — so the capture proves
    /// the expander's real wire, not a simplified one.
    template <class Slot>
    void encodeLoopbackFrameShift(uint8_t* frame, const uint8_t* wire, uint8_t outCh,
                                  nrOfLightsType lights) {
        auto* out = reinterpret_cast<Slot*>(frame);
        // The pattern rides `loopbackStrand` — whichever '595 output the jumper is actually on. It
        // need not be a strand the render loop drives: a spare output (e.g. the 16th of two '595s on
        // a 15-panel board) is the ideal tap, since nothing else loads it.
        const uint8_t s = (loopbackStrand < kMaxStrands) ? loopbackStrand : uint8_t{0};
        const uint64_t mask = uint64_t(1) << s;
        // `wire` holds ONE light's bytes at index 0; the shift encoder indexes it by strand, so
        // point strand `s`'s slot at those bytes.
        for (nrOfLightsType row = 0; row < lights; row++) {
            encodeWs2812ShiftSlots<Slot>(wire, mask, physPins_, latchBit_,
                                         outputsPerPin(), outCh, out);
            out += static_cast<size_t>(outCh) * 8 * 3 * outputsPerPin();
        }
        encodeWs2812ShiftLatchPad<Slot>(latchBit_, out);   // close the frame — see encodeRows
    }

    /// Test-only accessors — pin the lane slicing and frame-size arithmetic on the
    /// host (unit_{I80,Parlio}LedDriver.cpp); the hardware half is proven on device.
    uint8_t laneCount() const { return laneCount_; }
    /// Which DMA buffer this tick encodes into (0/1) — the deferred-wait double-buffer's alternation
    /// state. Test-only.
    uint8_t activeForTest() const { return active_; }
    /// Is buffer `i`'s DMA transfer outstanding (awaiting its wait)? Test-only.
    bool inFlightForTest(uint8_t i) const { return inFlight_[i]; }
    /// Lights on lane `i` (0 if out of range). Test-only.
    nrOfLightsType laneLightCount(uint8_t i) const { return i < laneCount_ ? laneCounts_[i] : 0; }
    /// All POPULATED strands the same length? Gates the ring's prefill-skip: what the skip actually
    /// requires is a per-row active mask that is constant across the frame — then a recycled buffer's
    /// prefilled constants stay valid across slices. An EMPTY lane (count 0) is in no row's mask at all,
    /// so it can never vary the mask: only the non-zero lane counts must agree. (The case that matters:
    /// a source that fills 15 of 16 expander strands is as cheap as a full uniform wall, not "ragged".)
    /// A truly ragged config re-prefills every refill (the mask varies per row region). Computed live —
    /// a handful of integer compares against the ~1/3-of-encode prefill it saves.
    // BENCH DIAGNOSTIC: per-segment cycle accumulators encodeRows fills at the shift branch —
    // gather+mask vs transpose+emit, per row. Static (shared across instances; one hot driver on the
    // bench). Plain uint32_t, NOT volatile: these are best-effort counters read-and-cleared once a second
    // by refreshBusKpi, so a lost/torn update under the two-core prime is harmless — and `volatile` is
    // both the wrong tool for cross-thread visibility (that is std::atomic's job) and, on a compound
    // assignment, deprecated in C++20 (-Wdeprecated-volatile). Scope: the backlog's ring entry.
    static inline uint32_t dbgSegGatherCy = 0;
    static inline uint32_t dbgSegEmitCy = 0;
    static inline uint32_t dbgSegRows = 0;

    bool MM_RAMFUNC uniformLaneCounts() const {
        nrOfLightsType ref = 0;
        for (uint8_t i = 0; i < laneCount_; i++) {
            if (laneCounts_[i] == 0) continue;   // empty lane: never in any mask
            if (ref == 0) ref = laneCounts_[i];
            else if (laneCounts_[i] != ref) return false;
        }
        return true;
    }
    /// First light index of lane `i`'s slice (0 if out of range). Test-only.
    nrOfLightsType laneStart(uint8_t i) const { return i < laneCount_ ? laneStart_[i] : 0; }
    /// Length of the longest lane — the frame's row count. Test-only.
    nrOfLightsType maxLaneLights() const { return maxLaneLights_; }
    /// Total DMA frame size in bytes (rows + latch pad). Test-only.
    size_t frameBytes() const { return frameBytes_; }

    // --- Peripheral-facing accessors: a backend reaches the orchestrator's parsed lane list, latch
    // bit, correction, and loopback pin through these (the back-pointer's read side — see owner_ in
    // LedPeripheral.h). Replaces what CRTP inheritance gave a derived class for free.
    /// Bus-bit index of the latch line (shift mode only) — a backend's addRingControls/encode seam
    /// reads this through the owner pointer (e.g. masking the latch bit out of a bit-verify).
    uint8_t latchBit() const { return latchBit_; }
    /// The parsed physical data GPIOs (bus-width bound) — a backend's encode trampoline reads lane
    /// state through the owner pointer instead of inheriting the array directly.
    const uint16_t* laneList() const { return laneList_; }
    /// The live output Correction (channel count, role wiring) — a backend's encode/prefill helpers
    /// read `outChannels` through this instead of inheriting `correction_` directly.
    const Correction& correction() const { return correction_; }
    /// Mutable reference to the ring-snapshot A/B knob, for a backend's addRingControls to bind
    /// (`controls.addControl("ringSnapshot", owner_->ringSnapshotRef())`) — a bool addControl binds by reference,
    /// so the control needs the member's address, not a copy.
    bool& ringSnapshotRef() { return ringSnapshot; }
    /// The ring snapshot buffer (null when unallocated / off the ring path) — a backend's KPI refresh
    /// (MoonLedDriver::refreshBusKpi) reads this to report the buffer's memory residency (PSRAM vs
    /// internal) through the owner pointer.
    const uint8_t* snapshotBuf() const { return snapshotBuf_; }
    /// The wired source buffer (null before setSourceBuffer) — same KPI-residency use as snapshotBuf().
    const Buffer* sourceBuffer() const { return sourceBuffer_; }

protected:
    // Fill peripheralOptions_/peripheralIndex_ from the registry, keeping only backends this chip can
    // run (lanesAvailable() > 0), and reconcile peripheralSel_ so it still points at the SAME backend
    // after the filter (or clamps into range). Built each defineDriverControls, so the Select always
    // offers exactly what the board supports. A backend is created briefly to read its lanesAvailable()
    // (a cheap object — no bus is brought up until busInit), then discarded.
    void buildPeripheralOptions() {
        // Remember which backend is selected now (by label) so we can re-find it after filtering.
        const char* current = (peripheralSel_ < peripheralOptionCount_) ? peripheralOptions_[peripheralSel_]
                                                                        : nullptr;
        peripheralOptionCount_ = 0;
        for (uint8_t i = 0; i < peripheralRegistryCount_ && peripheralOptionCount_ < kMaxPeripherals; i++) {
            LedPeripheral* probe = peripheralRegistry_[i].make();
            const bool usable = probe && probe->lanesAvailable() > 0;
            delete probe;
            if (!usable) continue;
            peripheralOptions_[peripheralOptionCount_] = peripheralRegistry_[i].label;
            peripheralIndex_[peripheralOptionCount_] = i;
            peripheralOptionCount_++;
        }
        if (peripheralOptionCount_ == 0) {   // no usable backend on this chip (desktop) — a single inert row
            peripheralOptions_[0] = "(none)";
            peripheralIndex_[0] = 0;
            peripheralOptionCount_ = 1;
        }
        // Re-point the Select at the same backend it named before (labels are stable), else clamp.
        uint8_t sel = 0;
        if (current)
            for (uint8_t k = 0; k < peripheralOptionCount_; k++)
                if (std::strcmp(peripheralOptions_[k], current) == 0) { sel = k; break; }
        peripheralSel_ = sel;
    }

    // Tear down the current backend and build the one at filtered option slot `k`. Frees the old
    // backend's bus + heap (only the selected peripheral costs memory — the product-owner requirement).
    // Guarded so a bad index or an empty registry leaves peripheral_ null (tick() then idles).
    void swapPeripheral(uint8_t k) {
        // Stop the core-1 encode worker before freeing the backend. deinit() drains the BUS DMA, but the
        // encode worker (owned by Drivers) ticks this driver and dereferences peripheral_ for
        // busBuffer/busTransmit/busWait — so a live `peripheral` swap (POST /api/control) that reaches
        // here while the render split is active would `delete peripheral_` out from under the worker
        // mid-tick: a use-after-free, the same class the structural-mutation quiesce fixes. The swap is
        // not a child-array mutation, so it does not pass through MoonModule::quiesceForMutation; reach
        // the same render-worker hook directly. The trailing reinit()/tick re-engages the split.
        //
        // Only when there is a PRIOR backend to free (`peripheral_` set). The hook exists to quiesce the
        // worker before the `delete peripheral_` below — so with no backend yet there is nothing to free
        // and nothing to guard. This matters because the FIRST swap of a fresh instance (the default-
        // peripheral selection in defineDriverControls) runs with `peripheral_` null: a throwaway probe
        // that /api/types builds via ModuleFactory to read a type's defaults constructs a ParallelLedDriver
        // and hits exactly that path. Firing the hook there tears down the LIVE Drivers' split (the hook
        // resolves to it via the static active() seat) for an instance that never had a backend — the "UI
        // refresh freezes the LEDs" bug, since /api/types runs on every page load. Gating on `peripheral_`
        // fires the notify for precisely the case it protects (a real swap that frees a live backend, whose
        // in-flight DMA the worker must be quiesced away from before the free) and skips the harmless
        // no-backend first build. (Not gated on parent(): a swap that frees a real backend must quiesce
        // regardless of tree attachment — the guard is "is there a backend to protect", not "am I live".)
        if (peripheral_) MoonModule::notifyQuiesceRender();
        deinit();                                   // stop any in-flight transfer on the old bus first
        if (peripheral_) {
            peripheral_->busDeinit();
            if (peripheralOwned_) delete peripheral_;   // never delete a test-borrowed mock
            peripheral_ = nullptr;
        }
        peripheralActiveReg_ = 0xFF;
        peripheralOwned_ = false;
        if (k >= peripheralOptionCount_) return;
        const uint8_t reg = peripheralIndex_[k];
        if (reg >= peripheralRegistryCount_) return;
        peripheral_ = peripheralRegistry_[reg].make();
        if (peripheral_) { peripheral_->attach(this); peripheralActiveReg_ = reg; peripheralOwned_ = true; }
    }

    // Seed the default peripheral for a registered driver (called from its constructor with the backend's
    // registry label). Builds the filtered options, points peripheralSel_ at `label` if this chip
    // supports it (else the first usable backend — a board that names an unavailable default still gets a
    // working driver), and swaps the live backend to match. From here the base's selector owns the rest.
    void selectDefaultPeripheral(const char* label) {
        buildPeripheralOptions();
        peripheralSel_ = 0;   // fallback: the first usable backend (a board naming an absent default
                              // still gets a working driver, and `label == nullptr` means "first").
        if (label)
            for (uint8_t k = 0; k < peripheralOptionCount_; k++)
                if (std::strcmp(peripheralOptions_[k], label) == 0) { peripheralSel_ = k; break; }
        swapPeripheral(peripheralSel_);
    }

    // Make the live backend match peripheralSel_. When a `peripheral` control change triggered a
    // control rebuild, peripheralSel_ already holds the new index but peripheral_ is still the old
    // backend — this swaps it so the schema below surfaces the right controls. A no-op when they
    // already agree (the common rebuild-for-another-reason case), so it's cheap on every rebuild.
    void ensurePeripheralMatchesSelection() {
        // A test-borrowed mock (setPeripheralForTest → !peripheralOwned_) is deliberate — never swap it
        // out from under the test for a registry backend.
        if (peripheral_ && !peripheralOwned_) return;
        const uint8_t wantReg = (peripheralSel_ < peripheralOptionCount_) ? peripheralIndex_[peripheralSel_]
                                                                          : 0xFF;
        if (peripheral_ && peripheralActiveReg_ == wantReg) return;   // already correct
        swapPeripheral(peripheralSel_);
    }

    // The peripheral-block claim guard: is a SIBLING driver under the same Drivers container already
    // driving the hardware block this driver's peripheral wants? The chip has one of each block (one
    // LcdCam, one Parlio, one I2S), so two live drivers on the same block corrupt each other. Returns
    // true when the block is already claimed. RTTI-free (ESP32 builds -fno-rtti): siblings are compared
    // through the virtual `hwBlock()` on every module — a non-parallel driver (RMT, NetworkSend) and any
    // other module return None, so no cast is needed. Walks the parent's children; skips self + disabled.
    bool siblingClaimsBlock() const {
        if (!peripheral_) return false;
        const LedHwBlock mine = peripheral_->hwBlock();
        if (mine == LedHwBlock::None) return false;
        const MoonModule* p = parent();
        if (!p) return false;
        for (uint8_t i = 0; i < p->childCount(); i++) {
            const MoonModule* sib = p->child(i);
            if (sib == this || !sib || !sib->enabled()) continue;
            // Every child of Drivers is a DriverBase (role Driver), so this static_cast is safe once
            // role() confirms it — no RTTI. A non-parallel driver returns None from hwBlock().
            if (sib->role() != ModuleRole::Driver) continue;
            if (static_cast<const DriverBase*>(sib)->hwBlock() == mine) return true;
        }
        return false;
    }

public:
    /// The hardware peripheral block this driver is DRIVING, for the sibling claim guard (overrides
    /// DriverBase). Gated on inited_: a driver reports its block only while it actually holds the bus,
    /// not merely from having a backend selected. Without this a driver that was itself refused (or has
    /// no pins yet) would still "claim" the block, so the next prepare sweep — re-running a working
    /// sibling's reinit() for an unrelated reason (a grid resize, a pin edit) — would see the phantom
    /// claim and dark the live wall too. The claim must mean "the bus is up", which is exactly inited_.
    /// siblingClaimsBlock() derives its OWN block from peripheral_ directly, so gating here only affects
    /// how OTHER drivers see this one — precisely the intent.
    LedHwBlock hwBlock() const override {
        return (inited_ && peripheral_) ? peripheral_->hwBlock() : LedHwBlock::None;
    }

protected:


    /// The runtime backend. Null only before a registered driver's constructor wires its default
    /// backend (or after a test detaches one) — every method that dereferences it guards first.
    LedPeripheral* peripheral_ = nullptr;

    // The `peripheral` Select's state. peripheralSel_ indexes into the BOARD-FILTERED option list
    // (only backends with lanesAvailable() > 0 on this chip), whose labels are held in peripheralOptions_
    // — a stable member array because addSelect borrows the pointer (same pattern as presetOptions_).
    // peripheralIndex_[k] maps filtered slot k back to its registry index, so a Select change can create
    // the right backend. All three are (re)built by buildPeripheralOptions().
    uint8_t peripheralSel_ = 0;
    const char* peripheralOptions_[kMaxPeripherals] = {};
    uint8_t peripheralIndex_[kMaxPeripherals] = {};
    uint8_t peripheralOptionCount_ = 0;
    uint8_t peripheralActiveReg_ = 0xFF;   // registry index the LIVE peripheral_ came from (0xFF = none)
    bool peripheralOwned_ = false;         // does the orchestrator own peripheral_ (delete it) — false
                                           // for a test-borrowed mock (setPeripheralForTest)

    Buffer* sourceBuffer_ = nullptr;

    LedDriverConfig cfg_;
    bool inited_ = false;
    uint8_t* dmaBuf_ = nullptr;          // platform-owned buffer 0; cached as the "inited" flag
                                         // for tick()'s guard (the per-tick encode target is
                                         // busBuffer(active_), which alternates 0/1)
    uint8_t active_ = 0;                 // which DMA buffer this tick encodes into (0/1); stays 0
                                         // in single-buffer mode (no second buffer allocated)
    bool inFlight_[2] = {};              // is buffer i's DMA transfer outstanding (awaiting its wait)
    // Consecutive frames whose DMA transfer never completed within its budget. NOT the `stallUs`
    // render-split KPI (which is a healthy core-0 wait) — this counts DEAD transfers, and is 0 on any
    // working bus. See busGaveUp(): a bus that keeps failing gets switched off rather than allowed to
    // spend the render thread and starve the network.
    uint8_t deadFrames_ = 0;
    bool gaveUpReported_ = false;        // one status write per give-up, not one per tick
    // setStatus stores the POINTER, so the text has to outlive the call; and the message is written
    // once per over-capacity episode, not once per tick, since it would otherwise rewrite the status
    // at frame rate for as long as the count stays too high.
    /// ONE buffer for every formatted status this driver reports (over-capacity, reserved bus pin).
    /// A member rather than a local because setStatus keeps the POINTER and does not copy
    /// (MoonModule.h), so stack storage would dangle the moment the formatting call returns. Shared
    /// rather than one per message: they are mutually exclusive cold-path refusals, each formatted
    /// immediately before its own setStatus, and 96 bytes per variant adds up on a classic ESP32.
    char statusBuf_[96] = {};
    bool overCapReported_ = false;
    static constexpr uint8_t kDeadFramesBeforeGiveUp = 8;
    // Given-up retry cadence: once given up, let one frame try every this-many ticks so a TRANSIENT stall
    // self-recovers without a reinit (see busGaveUp). ~50 ticks ≈ 1 s of retries — rare enough not to
    // starve the network with dead-frame waits, frequent enough that recovery feels immediate.
    uint16_t giveUpRetry_ = 0;
    static constexpr uint16_t kGiveUpRetryTicks = 50;
    // Per-row correction scratch (wire_ / wireCap_ live on DriverBase — the grow-only lifecycle is
    // shared with RmtLedDriver; only the SIZE differs). Here it is kMaxStrands × outChannels bytes
    // PER CORE (kMaxCores slices; encodeRows indexes its own core's slice — per-CPU data),
    // lane-major (wire_[lane*outCh+ch]) — sized to the channel count off the hot path, so a light of
    // any channel count fits (RGB=3, RGBW=4, RGBCCT=5, an N-channel fixture; limit is memory). A fixed
    // 4-byte-stride stack array here overflowed for >4-channel corrections → the SE16 bootloop.
    char frameTimeStr_[40] = "—";             // read-only `frameTime` KPI text (refreshed in tick1s); sized
                                         // for the worst case "<us> µs (<fps> fps max)" + the 3-byte
                                         // UTF-8 µ, so the snprintf never truncates (-Wformat-truncation)
    uint16_t laneList_[kMaxLanes] = {};              // physical data GPIOs (bus width bound)
    uint16_t busPinBuf_[kMaxLanes] = {};             // data pins + latch — the list busInit() builds from
    nrOfLightsType laneCounts_[kMaxStrands] = {};    // per-STRAND light count (expander bound)
    nrOfLightsType laneStart_[kMaxStrands] = {};     // per-STRAND offset into the window
    nrOfLightsType winStart_ = 0;   // first source-buffer light this driver reads (the window)
    nrOfLightsType winLen_ = 0;     // window length (lights), clamped to the buffer
    uint8_t laneCount_ = 0;      // STRAND lanes driven = physPins_ × outputsPerPin() (expanded)
    uint8_t physPins_ = 0;       // physical data GPIOs (== laneCount_ in direct mode)
    uint8_t latchBit_ = 0;       // bus-bit index of the latch line (shift mode only)
    nrOfLightsType maxLaneLights_ = 0;
    size_t frameBytes_ = 0;

    // Per-frame source SNAPSHOT for the streaming ring. The ring's refill encodes OFF the render thread,
    // concurrently with the render loop overwriting the source buffer (the Drivers output buffer, or the
    // layer buffer in the zero-copy identity case), so it must read an immutable copy. Only THIS DRIVER'S
    // WINDOW is copied — `winLen_ × srcCh` bytes from `winStart_`, not the whole source — so on a large
    // display split across many drivers each copies only the slice it reads, not the entire frame. (The
    // source is the raw pixel array: 3 B/light, so 48 KB at 16K lights — the window keeps a per-driver
    // copy proportional to that driver's strands, ~11.5 KB for a 16×256 driver.) snapshotSourceForRing()
    // fills it at transmit time and points encodeSrc_ at it (biased by -winStart_ so encodeRows' index is
    // unchanged); encodeRows reads encodeSrc_ when set. Grow-only, freed in release() with the scratch.
    uint8_t* snapshotBuf_ = nullptr;      // driver-owned copy of the source WINDOW (ring only)
    size_t   snapshotCap_ = 0;            // allocated capacity, grows to fit the window

    /// This driver's heap = the base scratch + the streaming snapshot (the ring's immutable frame copy,
    /// the biggest single driver buffer at ~36 KB) + the peripheral's DMA buffers. Summed for the
    /// per-module memory readout (see DriverBase::driverHeapBytes).
    ///
    /// The DMA buffers are allocated by the PLATFORM rather than by this driver, and they used to be
    /// left out on that ownership argument. That made the readout lie about the thing a user actually
    /// decides on: the i80 frame is sized by the BUS WIDTH (8 or 16 lanes) and not by the pins in use,
    /// so a one-lane board pays the same ~50 KB as an eight-lane one. On the bench (2026-09-08) a
    /// QuinLED Dig-2-Go showed 512 bytes for this driver against RmtLedDriver's 32 KB while actually
    /// costing 49 KB MORE free heap, and nothing on the card said so. Ownership is the wrong question
    /// for a memory readout: what the user needs is what choosing this driver costs.
    size_t driverHeapBytes() const override {
        size_t dma = 0;
        if (peripheral_) {
            const size_t cap = peripheral_->busCapacity();
            // Count the buffers that EXIST, not the ones `doubleBuffer` asks for: the control
            // defaults on, a peripheral may refuse the second allocation (or not support it at
            // all), and a readout that trusts the request over the allocation reports memory the
            // board never spent.
            dma = cap;
            if (peripheral_->busBuffer(1)) dma += cap;
        }
        return DriverBase::driverHeapBytes() + snapshotCap_ + dma;
    }
    // The snapshot copy's inputs, set before copyRange runs. The snapshot is serial (on the ring's core-1
    // tick); only the ring PRIME still forks to the core-0 helper (busTransmitRing), so no cross-core copy
    // bounds live here.
    const uint8_t* snapCopySrc_ = nullptr;
    uint8_t  snapCopyCh_ = 0;
    const uint8_t* encodeSrc_ = nullptr;  // when non-null, encodeRows reads this (bias-corrected) instead of sourceBuffer_

    /// (Re)size the ring snapshot to hold this driver's whole window (`winLen_ × srcCh`), OFF the hot path.
    /// Called from the ring build in reinit(), so snapshotSourceForRing() on the render thread only memcpys
    /// — never allocates (the hot-path no-alloc rule). Grow-only; a larger window later re-grows here, not
    /// mid-frame. Returns false if it can't allocate (the ring build then degrades like any alloc failure).
    bool ensureSnapshotCap() {
        if (!sourceBuffer_) return true;   // sized on the first build that has a source; harmless if absent
        // The snapshot holds a RAW COPY of the source window at SOURCE channel stride — a plain memcpy,
        // correction fused into encodeRows' gather (not pre-applied here). So size by the SOURCE channel
        // count. srcCh is the live buffer's; fall back to outCh before a source is wired (harmless, the
        // real size lands on the first build that has one). ~12 KB at 16×256 RGB, ~36 KB at 48×256.
        const size_t srcCh = sourceBuffer_->channelsPerLight();
        const size_t bytes = static_cast<size_t>(winLen_) * (srcCh ? srcCh : correction_.outChannels);
        if (bytes == 0 || snapshotCap_ >= bytes) return true;
        // INTERNAL RAM first: the ring's ISR refill reads this buffer per byte, and a PSRAM-resident
        // snapshot pays PSRAM latency hundreds of times per slice (measured ~10% of a 595 µs refill). PSRAM
        // fallback keeps a RAM-tight board rendering (slow beats dark); the internal window is modest
        // (window lights × outCh, e.g. ~12 KB at 16×256 RGB, ~36 KB at 48×256).
        uint8_t* grown = static_cast<uint8_t*>(platform::allocInternal(bytes));
        if (!grown) grown = static_cast<uint8_t*>(platform::alloc(bytes));
        if (!grown) return false;
        if (snapshotBuf_) platform::free(snapshotBuf_);
        snapshotBuf_ = grown;
        snapshotCap_ = bytes;
        this->publishHeapBytes();   // the snapshot grew — refresh the memory readout
        return true;
    }

    /// Freeze THIS DRIVER'S WINDOW of the source into the driver-owned snapshot as a RAW MEMCPY (source
    /// bytes at srcCh stride) — and route the ring's encode at it. The refill (off the render thread) then
    /// reads a frozen frame: no use-after-free on a resize, no tearing, while the render loop is free to
    /// overwrite the live buffer. Correction is NOT applied here — it fuses into encodeRows' gather (one
    /// pass, near-free on the ISR; the memcpy keeps the immutability the snapshot exists for). NO
    /// ALLOCATION here — the buffer was sized in reinit() (ensureSnapshotCap, srcCh stride). Returns false
    /// (encodeSrc_ null, caller bails) if the source is missing, the window is empty, or it wasn't sized.
    /// Sets encodeSrc_ = snapshotBuf_ - winStart_ * srcCh, so encodeRows' index (winStart_ + laneStart_ +
    /// row) lands in the windowed copy WITHOUT changing the formula (at the snapshot's srcCh stride). The
    /// bias pointer is only ever dereferenced at indices ≥ winStart_ * srcCh, so it never reads OOB.
    bool snapshotSourceForRing() {
        encodeSrc_ = nullptr;
        if (!sourceBuffer_ || !sourceBuffer_->data() || !snapshotBuf_) return false;
        const size_t srcCh = sourceBuffer_->channelsPerLight();
        const size_t outCh = correction_.outChannels;
        if (srcCh == 0 || outCh == 0) return false;
        // Clamp to what the live buffer AND the sized snapshot both hold: winLen_/winStart_ come from
        // config and the source can have shrunk since reinit (a grid resize past the render thread is
        // exactly the hazard this snapshot guards), so never read past the source nor write past snapshotCap_.
        const nrOfLightsType count = sourceBuffer_->count();
        if (winStart_ >= count) return false;
        nrOfLightsType winLights = (winStart_ + winLen_ > count)
                                       ? static_cast<nrOfLightsType>(count - winStart_)
                                       : winLen_;
        // Clamp against the SOURCE stride — the snapshot is a raw srcCh-strided memcpy (ensureSnapshotCap
        // sizes winLen_ × srcCh, copyRange writes at srcCh). Using outCh here would drop the window's tail
        // whenever outCh > srcCh (RGB source → RGBW correction), reading stale bytes for those lights.
        if (static_cast<size_t>(winLights) * srcCh > snapshotCap_)
            winLights = static_cast<nrOfLightsType>(snapshotCap_ / srcCh);   // never overrun the sized buffer
        if (winLights == 0) return false;
        // The snapshot is a PLAIN MEMCPY of the live window (raw source bytes at srcCh stride) — fast,
        // and the immutability is the whole point (the ISR refill reads a frozen frame while the render
        // loop is free to overwrite the live buffer). Correction is NOT applied here; it fuses into
        // encodeRows' gather (one pass, near-free on the ISR — measured). The copy is still forkable:
        // when the render/encode split is active there's an idle second core, so hand it the bottom half.
        // Off the split (or if the helper can't run) this collapses to one full-range copy — byte-identical.
        // Line-aligned split so neither core writes into a cache line the other also writes.
        snapCopySrc_ = sourceBuffer_->data() + static_cast<size_t>(winStart_) * srcCh;
        snapCopyCh_ = static_cast<uint8_t>(srcCh);
        // SERIAL copy, always — on core 1 under the split (the whole ring tick runs there). The snapshot is
        // a ~3 ms memcpy; forking its bottom half onto the core-0 helper saved ~1.5 ms but cost far more: the
        // helper is core-0 priority-5 work stacked on the render's composite, and at 48×256 the two SATURATE
        // core 0 (measured: the idle task starved for >5 s at engage → a silent hang, the whole-session
        // freeze). It was ALSO a correctness hazard — the snapshot and prime forks shared one helper's job
        // and bounds, and a mid-frame re-kick tore the last lanes (the bottom-16 flicker). Only the PRIME
        // still forks (busTransmitRing): the prime buffers are independent and the prime is the real win.
        copyRange(0, winLights);
        // INTRUSIVE loopback pattern hold: overwrite the tapped strand's rows in the snapshot with the test
        // pattern in RAW SOURCE bytes (correction runs later in encodeRows, uniformly). Window-relative
        // index, clamped to the lights the copy filled. Off (-1) is the normal render path.
        if (patternHoldStrand_ >= 0 && static_cast<uint8_t>(patternHoldStrand_) < laneCount_) {
            const uint8_t lane = static_cast<uint8_t>(patternHoldStrand_);
            uint8_t patSrc[8] = {};   // pattern in SOURCE channels (corrected downstream like every light)
            // Clamp to patSrc's capacity: a fixture with srcCh > 8 (multi-channel modes) would otherwise
            // memcpy past the pattern buffer. The extra channels stay zero in the snapshot, which is the
            // intended hold value for anything past RGB anyway.
            const size_t patCh = srcCh < sizeof(patSrc) ? srcCh : sizeof(patSrc);
            for (size_t ch = 0; ch < patCh; ch++)
                patSrc[ch] = ch < 3 ? kPatternRGB_[ch] : uint8_t{0};
            const nrOfLightsType laneRows = laneCounts_[lane];
            for (nrOfLightsType row = 0; row < laneRows; row++) {
                if (static_cast<nrOfLightsType>(laneStart_[lane] + row) >= winLights) break;
                std::memcpy(snapshotBuf_ + (static_cast<size_t>(laneStart_[lane]) + row) * srcCh,
                            patSrc, patCh);
            }
        }
        // Bias so the unchanged index (winStart_ + laneStart_ + row) — at srcCh stride — addresses into
        // the windowed copy: snapshotBuf_[0] holds the light at winStart_. encodeRows reads it as a live
        // source (raw srcCh bytes) and corrects per light.
        encodeSrc_ = snapshotBuf_ - static_cast<size_t>(winStart_) * srcCh;
        return true;
    }

    /// Copy lights [lo, hi) of the current window into snapshotBuf_ — a raw memcpy at srcCh stride, the
    /// parallelizable body of the snapshot, run whole (serial) or one half per core (the fork-join). Reads
    /// only the snapCopy* fields the orchestrator set; disjoint [lo,hi) ranges never touch the same bytes.
    /// MM_RAMFUNC: both cores run it, so keep it out of the shared flash cache.
    void MM_RAMFUNC copyRange(nrOfLightsType lo, nrOfLightsType hi) {
        const size_t ch = snapCopyCh_;
        if (hi > lo)
            std::memcpy(snapshotBuf_ + static_cast<size_t>(lo) * ch, snapCopySrc_ + static_cast<size_t>(lo) * ch,
                        static_cast<size_t>(hi - lo) * ch);
    }


    /// Split winLights near the middle, rounded so the second half starts on a 64-byte cache line — each
    /// half then owns whole lines, so the two cores never write into a shared line (false sharing). The
    /// line holds `64 / outCh` lights (rounded down); align the boundary to a whole number of them.
    static nrOfLightsType snapLineAlignedHalf(nrOfLightsType winLights, size_t chStride) {
        if (chStride == 0) return winLights / 2;
        // The MINIMAL light count whose byte extent is a whole number of 64-byte cache lines is
        // 64 / gcd(64, stride) lights (stride 3 → 64 lights = 3 lines; stride 4 → 16 lights = 1 line).
        // A plain 64/stride rounds DOWN (stride 3 → 21 lights = 63 bytes) and misses the line boundary,
        // silently re-introducing the false sharing this split exists to avoid.
        const auto lightsPerAlign = static_cast<nrOfLightsType>(64 / std::gcd(size_t{64}, chStride));
        nrOfLightsType half = winLights / 2;
        half -= half % lightsPerAlign;                                // round DOWN to a line boundary
        if (half == 0) half = lightsPerAlign;                         // never give the helper nothing
        return half < winLights ? half : winLights / 2;               // tiny window: plain (unaligned) halve
    }
    // INTRUSIVE loopback pattern hold: which strand carries the pinned pattern (-1 = off), and the RGB bytes
    // (the recognisable 0xA5/0x00/0xFF the bit-verify expects). Set around the capture in runIntrusiveLoopback.
    int16_t patternHoldStrand_ = -1;
    uint8_t kPatternRGB_[3] = {0xA5, 0x00, 0xFF};

    /// The two wirings that exist: strands on the GPIOs, or a 74HCT595 per GPIO.
    uint16_t busPins_[kMaxLanes] = {};   // data pins the live bus/unit was built
                                         // with — a pin change must rebuild even
                                         // when the buffer still fits
    uint8_t  busLaneCount_ = 0;          // lane count the live bus was built with —
                                         // a lane-count change (e.g. Parlio 8→4)
                                         // can keep the same frameBytes_ yet needs
                                         // a rebuild, so the fast path checks it too

    /// The pin-list parser's cap: the peripheral's own lane count when it's narrower than kMaxLanes
    /// (Parlio may report fewer), else the full kMaxLanes. Falls back to kMaxLanes with no peripheral
    /// attached yet (parseConfig then has nothing to parse anyway).
    uint8_t maxLanesForTarget() const {
        const uint8_t avail = peripheral_ ? peripheral_->lanesAvailable() : 0;
        return (avail > 0 && avail < kMaxLanes) ? avail : kMaxLanes;
    }

    // Frame bytes: longest lane × channels × 24 slots, plus a zeroed latch pad of
    // >=300 µs at the slot rate (800 slots) with clock-tolerance slack (64), each
    // scaled by `slotBytes` (1 for an 8-bit bus, 2 for a 16-bit bus — a slot is one
    // bus WORD, so a 16-lane frame is 2 bytes/slot), rounded up to the bus's 64-byte
    // alignment. 0 when there's nothing to send. The pad scales too: it's a count of
    // idle bus words, and its LOW duration is measured in slots, so an unscaled pad
    // would be half the latch time on a 16-bit bus and could latch a strand mid-frame.
    //
    // `outPerPin` (1, or kPinExpanderOutputs with a '595 expander) multiplies the ROW slots: a
    // shift register is serial-in, so presenting each WS2812 slot costs outPerPin shift
    // cycles, each its own bus word. The latch pad is NOT multiplied — it is a LOW-time
    // measured in bus words, and the bus already clocks outPerPin× faster in shift mode,
    // so an unscaled pad holds the same wall-clock idle... which would be outPerPin× too
    // SHORT. Scale it by outPerPin to keep the ≥300 µs strand-latch window intact.
    // Bytes ONE light-row encodes to: outCh channels × 24 slots (3 per bit × 8 bits) × slotBytes ×
    // outPerPin shift words. The single source of the row geometry — frameBytesFor and the streaming
    // ring both derive from it, so the ring's per-slice size can never drift from the whole-frame size.
    static size_t rowBytesFor(uint8_t outCh, uint8_t slotBytes, uint8_t outPerPin) {
        return static_cast<size_t>(outCh) * 24 * slotBytes * outPerPin;
    }
    // The trailing latch pad: 800 idle bus words (≥300 µs at the slot rate) + 64 clock-tolerance slack,
    // scaled by slotBytes and outPerPin (see the note above — the shift bus clocks faster, so the pad
    // must scale to hold the same wall-clock LOW). One frame carries exactly one pad, at its end.
    static size_t padBytesFor(uint8_t slotBytes, uint8_t outPerPin) {
        return static_cast<size_t>(800 + 64) * slotBytes * outPerPin;
    }

    static size_t frameBytesFor(nrOfLightsType maxLights, uint8_t outCh, uint8_t slotBytes,
                                uint8_t outPerPin = 1) {
        if (maxLights == 0 || outCh == 0 || outPerPin == 0) return 0;
        const size_t bytes = static_cast<size_t>(maxLights) * rowBytesFor(outCh, slotBytes, outPerPin)
                             + padBytesFor(slotBytes, outPerPin);
        return (bytes + 63) & ~static_cast<size_t>(63);
    }

    // Whether a whole-frame DMA buffer of `frameBytes` fits the internal-DMA budget of a driver whose
    // DMA can't reach PSRAM (the classic i80's I2S peripheral) and holds the whole frame (no ring).
    // A driver that IS so bounded calls this in reinit() (before busInit); if false it idles with a clear
    // status instead of choking the bus init on an allocation the peripheral can never satisfy. `budgetBytes`
    // 0 means "no bound" (PSRAM-capable / ring drivers) → always fits.
    static bool frameFitsDmaBudget(size_t frameBytes, size_t budgetBytes) {
        return budgetBytes == 0 || frameBytes <= budgetBytes;
    }

    // The i80 bus width is a POWER OF TWO (8 or 16) — a peripheral fact, independent of how many
    // strands the board drives. In direct mode the pin list already fills it exactly. In shift mode
    // the board decides the data-pin count (how many '595s are populated), so the driver rounds up to
    // the next legal width and pads the spare lanes itself.
    uint8_t busWidthPins() const {
        // Data pins, plus the latch lane when a '595 expander is in use.
        const uint8_t needed = static_cast<uint8_t>(physPins_ + (pinExpanderMode() ? 1 : 0));
        return needed <= 8 ? uint8_t{8} : uint8_t{16};
    }

    // The GPIO list the PERIPHERAL is built from — always busWidthPins() entries long, because the bus
    // is 8 or 16 bits wide however many pins the board actually drives:
    //   - bus bits 0..physPins_-1 are the data pins (bit L = the L-th entry of `pins`, the
    //     ParallelSlots contract),
    //   - in shift mode, bus bit latchBit_ (== physPins_) is the LATCH — a real lane the peripheral
    //     drives,
    //   - every REMAINING lane up to the bus width is parked on the WR/clock pin. The i80 layer
    //     rejects an NC data pin, so a spare lane must be *some* real GPIO; WR is the safe choice
    //     because the peripheral already drives it and the board already wires it (hpwit's trick,
    //     and exactly what platform_esp32_i80 does for lanes beyond laneCount). Those lanes carry no
    //     strand — their bits are never set in activeMask — so they are inert.
    // The padding is what lets a board name ONLY the pins it drives, at any count: an 8-bit bus with 5
    // strands is 5 data lanes and 3 parked on WR. Returning the raw laneList_ in direct mode would hand
    // the peripheral a short array while busPinCount() claims the full width — a read past the end.
    // Kept in the base (one owner of the latch + the padding), so both derived busInit()s just pass
    // these two calls.
public:
    /// Public: a backend (a separate LedPeripheral, reached through the owner_ back-pointer) builds its
    /// bus from this + busPinCount()/busClockMultiplier(), same as busPinList()/busPinCount()/slotBytes()
    /// below — the bus-geometry accessors a backend needs are public, everything else in this block stays
    /// protected (this driver's own cold-path config machinery).
    const uint16_t* busPinList() {
        const uint8_t width = busPinCount();
        const uint16_t clockPin = peripheral_ ? peripheral_->clockPinForBus() : laneList_[0];
        for (uint8_t i = 0; i < width && i < kMaxLanes; i++) {
            if (i < physPins_)                        busPinBuf_[i] = laneList_[i];   // data
            else if (pinExpanderMode() && i == latchBit_)   busPinBuf_[i] = static_cast<uint16_t>(latchPin);
            else                                      busPinBuf_[i] = clockPin;
        }
        return busPinBuf_;
    }
    /// How many lanes the PERIPHERAL is handed. The bus is 8 or 16 bits wide whatever the board wires,
    /// but only a backend that cannot leave a lane unconnected needs a pad for the spares: `esp_lcd`
    /// rejects an NC data pin and parks them on WR, while MoonI80 routes its own GPIOs and simply does
    /// not connect them (spareLanesNeedPad). Handing MoonI80 only the real lanes is what keeps a
    /// one-strand board from driving six or seven GPIOs it never asked for, one of which, on an S31,
    /// is an Ethernet transmit line.
    uint8_t busPinCount() const {
        const uint8_t width = busWidthPins();
        if (peripheral_ && !peripheral_->spareLanesNeedPad()) {
            const uint8_t real = static_cast<uint8_t>(physPins_ + (pinExpanderMode() ? 1 : 0));
            return real < width ? real : width;
        }
        return width;
    }
    // Bus clock: a '595 must be fed kPinExpanderOutputs shift cycles per WS2812 slot, so the bus clocks
    // that much faster to hold the same 375 ns slot on the wire. The platform picks the exact rate
    // its clock tree can divide to (see platform_esp32_i80.cpp); this is the multiplier.
    uint8_t busClockMultiplier() const { return outputsPerPin(); }
    // Bytes per bus slot: 1 for the 8-bit bus, 2 for the 16-bit bus. Keys on the PHYSICAL
    // pin count, not laneCount_ — with a '595 expander the lanes ride the shift cycles, not
    // extra bus bits, so 48 lanes on 6 pins is still an 8-bit bus. (In direct mode the two
    // counts are equal, so this is the original behaviour.) A backend's encode trampoline
    // (MoonLedDriver::ringEncodeTrampoline) branches on this through the owner_ pointer.
    uint8_t slotBytes() const { return busWidthPins() > 8 ? 2 : 1; }
protected:

    // (Re)size the per-row correction scratch to kMaxLanes × outCh bytes. Grows-only, off the hot
    // path (called from parseConfig). Sizing to outCh — not a fixed 4 — is what lets encodeRows lay
    // out any channel count without overrun. Failure leaves wire_ null; tick() then idles.
    void prepareWire(uint8_t outCh) {
        // Sized for STRANDS, not bus lanes: with a '595 expander the wire block is indexed by
        // expanded lane (up to kMaxStrands), which is what encodeRows fills and the shift encoder
        // reads. Grows-only, so a direct-mode driver that later enables the expander just grows.
        // × kMaxCores: one scratch slice per core (per-CPU data), because the ring's prime fork-join
        // runs encodeRows on BOTH cores concurrently — a single shared slice scatters the frame.
        ensureWire(static_cast<size_t>(kMaxStrands) * (outCh ? outCh : 1) * platform::kMaxCores);
    }

    // Re-derive lanes/counts/starts/frame size from the controls and the wired
    // buffer/correction. Off the hot path; on error the driver idles with the
    // parse literal in the status slot (same shape as RmtLedDriver).
    bool parseConfig() {
        laneCount_ = 0;
        physPins_ = 0;
        maxLaneLights_ = 0;
        frameBytes_ = 0;
        uint8_t n = 0;
        const char* err = parsePinList(pins, laneList_, maxLanesForTarget(), n);
        // (Nothing to validate about the fan-out itself: pinExpander is a bool, so the only two
        // wirings that physically exist are the only two it can express.)
        // The shift-register expander needs a backend that can DMA the 8× frame from PSRAM: the LCD_CAM
        // i80 path (S3 / P4). On a peripheral that can't (classic-ESP32 i80 = internal-DMA-only I2S, or
        // Parlio's 65,535 B single-transfer cap), SILENTLY drop back to direct mode rather than erroring:
        // the `pinExpander` control is HIDDEN on such a peripheral (see supportsPinExpander), so a user
        // who lands here — via a saved config or a peripheral switch — has no toggle to turn it off. An
        // unfixable error status is the wrong answer; degrade to the working direct path (robust-to-any-input).
        // The degrade is gated in pinExpanderMode() (the EFFECTIVE mode), NOT by mutating the stored
        // `pinExpander` — so A/B'ing a MoonI80 '595 wall through Parlio and back keeps the saved value, and
        // it comes up in shift mode again rather than silently reverting to direct (the same "don't mutate
        // a persisted value to express a runtime condition" lesson as the backend-control persistence fix).
        //
        // The latch is a real GPIO and a real bus bit; without it the '595s never present a byte.
        if (!err && pinExpanderMode() && latchPin < 0) err = "the 74HCT595 expander needs a latchPin";
        // **The BUS width is a peripheral fact; the PIN COUNT is a board fact. They are not the same
        // number, and the driver — not the user — reconciles them.** The i80 bus is 8 or 16 bits wide
        // (lcd_ll_set_data_wire_width takes nothing else), but nothing says every bit must reach a
        // GPIO: the matrix routes each data signal wherever we point it, and busPinList() parks the
        // spare lanes on WR, where the peripheral already drives and nothing reads them. So the user
        // configures only the pins that drive something, at ANY count, and the driver rounds the bus
        // up around them.
        //
        // In shift mode the LATCH also occupies a bus bit, so it costs one of the width's lanes —
        // hence kMaxLanes - 1 data pins there against kMaxLanes here.
        if (peripheral_ && peripheral_->powerOfTwoBus()) {
            const uint8_t maxData = static_cast<uint8_t>(kMaxLanes - (pinExpanderMode() ? 1 : 0));
            if (!err && (n == 0 || n > maxData))
                err = pinExpanderMode() ? "shift mode needs 1..15 data pins (one per populated 74HCT595)"
                                  : "i80 bus needs 1..16 pins";
        }
        // The latch drives its own bus bit, so it must not double as a data pin (that lane would
        // carry the latch waveform instead of pixel data). clockPin/dcPin overlap is the derived
        // driver's validateBusPins/validateBusFatal job, and the latch is checked against them there.
        if (!err && pinExpanderMode()) {
            for (uint8_t i = 0; i < n; i++)
                if (laneList_[i] == static_cast<uint16_t>(latchPin)) {
                    err = "latchPin collides with a data pin";
                    break;
                }
        }
        // Fatal bus-pin misconfig (MultiPinLedDriver's clockPin==dcPin — the i80 bus can't init) → the
        // error path below, which idles the driver. Checked before the per-lane WARNINGS: a broken
        // bus is worse than a garbled lane, so it wins the status.
        if (!err && peripheral_) err = peripheral_->validateBusFatal();
        // Peripheral-specific data-pin check (i80's WR/DC on a data lane routes two
        // output signals to one pin, so that lane emits the clock/DC waveform, not
        // pixel data). This is a WARNING, not a blocker: on a board that wires all
        // 8/16 lanes but drives fewer strands, an unused data pin is a legitimate
        // WR/DC candidate — only the lanes that actually drive a strand would show
        // garbage, and the user opted into that. Parlio has no WR/DC and returns null.
        // Kept a peripheral hook so the base stays backend-neutral.
        const char* warn = (err || !peripheral_) ? nullptr : peripheral_->validateBusPins(laneList_, n);
        // STRAND lanes: each physical pin fans out to outputsPerPin() strands through its '595.
        // This is the count ledsPerPin distributes over and the encoder indexes — from here on
        // "lane" means a strand, and physPins_ holds the GPIO count.
        const uint8_t lanes = static_cast<uint8_t>(n * outputsPerPin());
        if (!err && lanes > kMaxStrands) err = "too many strands (pins × 8 through the expander)";
        if (!err) {
            // Distribute over this driver's window slice, not the whole buffer.
            const nrOfLightsType bufN = sourceBuffer_ ? sourceBuffer_->count() : 0;
            windowSlice(bufN, winStart_, winLen_);
            const char* clampWarn = nullptr;
            // assignCounts clamps each lane to kMaxWs2812LedsPerPin (drives that many
            // rather than choking a whole grid onto one WS2812 line). Its own warning
            // (a clamped lane) wins over the WR/DC one only if it sets warn non-null.
            err = assignCounts(ledsPerPin, lanes, winLen_, laneCounts_, kMaxWs2812LedsPerPin,
                               &clampWarn);
            if (clampWarn) warn = clampWarn;
        }
        if (err) {
            setConfigErr(err);
            return false;
        }
        physPins_ = n;
        laneCount_ = lanes;
        // The latch takes the bus bit just above the data pins (data pins are bus bits 0..n-1,
        // matching the ParallelSlots contract that bus bit L = the L-th entry of `pins`).
        latchBit_ = n;
        nrOfLightsType start = 0;
        for (uint8_t i = 0; i < laneCount_; i++) {
            laneStart_[i] = start;
            start = static_cast<nrOfLightsType>(start + laneCounts_[i]);
            if (laneCounts_[i] > maxLaneLights_) maxLaneLights_ = laneCounts_[i];
        }
        const uint8_t outCh = correction_.outChannels;
        // physPins_ and pinExpander are set above, so slotBytes() reflects the real BUS width (data pins
        // + the latch bit), not the strand count — 48 lanes on 6 pins is still an 8-bit bus. The
        // ×8 lands in the slot COUNT instead (outputsPerPin()), which is what grows the frame.
        frameBytes_ = frameBytesFor(maxLaneLights_, outCh, slotBytes(), outputsPerPin());
        overCapReported_ = false;   // a new geometry re-earns its verdict: lowering the count recovers
        // Size the per-row correction scratch to kMaxLanes × outCh (grows-only, off the hot path).
        // Stride outCh, so any channel count fits without the old fixed-4-byte overflow.
        prepareWire(outCh);
        clearConfigErr();
        // A lane clamped to the WS2812 ceiling still drives — Warning, not error (see RmtLed).
        setConfigWarn(warn);
        // With nothing more urgent to show AND lights actually driven, report how many
        // this driver consumes (Σ laneCounts_ = `start`) of its window — so the user sees
        // real consumption instead of guessing from grid × pins. A clamp warning wins; an
        // idle driver (no pins / empty buffer) stays statusless, not "driving 0 of 0".
        if (!warn && start > 0) setDrivingInfo(start, winLen_, outCh);
        return true;
    }

    // --- bus + DMA buffer (hardware; this peripheral's targets only). Fused on
    // purpose: max_transfer_bytes is fixed at bus creation, so growing the frame
    // means re-creating the bus and its buffer together. Grow-only; the buffer is
    // re-zeroed on every reinit so a shrunken frame's latch pad can't hold stale
    // row bytes. ---

    void reinit() {
        if (!peripheral_ || peripheral_->lanesAvailable() == 0) return;
        // Peripheral-block claim guard: the chip has ONE of each block (one LcdCam, one Parlio, one
        // I2S), so a second live driver on the same block would corrupt the first's DMA. If a sibling
        // already holds this peripheral's block, idle with a clear status instead of fighting over the
        // hardware — the same fail-clean spirit as the DMA-budget gate. (Two DIFFERENT peripherals —
        // RMT + Parlio + i80 on a P4 — coexist fine; only same-block collides.)
        if (siblingClaimsBlock()) {
            deinit();
            setConfigErr("peripheral already in use by another driver — pick a different one");
            return;
        }
        // A rebuild is the user fixing the setting that broke the bus: give the new bus a clean slate
        // so a driver that gave up starts transmitting again (the live-reconfiguration rule — no reboot).
        deadFrames_ = 0;
        gaveUpReported_ = false;
        giveUpRetry_ = 0;
        // Drain any in-flight DMA before touching the buffers: a rebuild (or the exact-match
        // re-zero below) must not race a transfer still reading the old buffer. No-op when idle.
        drainInFlight();
        if (laneCount_ == 0 || frameBytes_ == 0) { deinit(); return; }
        // Reuse the existing bus ONLY when the frame is the EXACT same size and on the same pins/lanes.
        // Grow OR shrink both rebuild: a peripheral whose single-transfer size is fixed at bus creation
        // (Parlio: max_transfer_size sets a hardware bit-length cap) becomes INVALID if the buffer is
        // reused at a different size — a shrunk frame kept an oversized unit whose configured max
        // exceeded the hardware frame limit, so every transmit silently failed (tx=0, no output, no
        // error). Exact-match reuse (not `>=`) keeps the bus always valid-or-rebuilt. The pin check
        // matters (a pin edit keeps the size but must move the GPIOs); the lane-count check matters for
        // Parlio (8→4 keeps frameBytes but the bus was built for 8 lanes).
        // Also rebuild when the second-buffer PRESENCE no longer matches doubleBuffer: toggling the
        // flag adds (ON) or frees (OFF) buffer 1, which only busInit/deinit can do. `haveSecond`
        // reflects what's currently allocated; `wantSecond` what the flag now asks for.
        // RING PATH (MoonI80, oversize shift mode): the whole-frame reuse/alloc logic below does not
        // apply — a ring holds no whole frame, so busCapacity() is one small slice and the exact-match
        // check would never fire anyway. Build the ring fresh; on failure fall through to the whole-frame
        // path, which then idles with a status if the frame won't fit either (the always-available
        // degrade). The ring owns its own buffers + refill task, so a plain deinit()+rebuild is correct;
        // ring-reuse is a later optimization (a config change already forces a rebuild).
        if (peripheral_->wantsRing()) {
            deinit();
            const uint8_t outCh = correction_.outChannels;
            // Rows only — a ring buffer carries no latch pad (the reset comes from stopping the
            // peripheral), so unlike the whole-frame path below there is no padBytesFor() here.
            const size_t rowBytes = rowBytesFor(outCh, slotBytes(), outputsPerPin());
            // The snapshot is sized HERE, off the hot path (tickRing only memcpys into it), and it is
            // load-bearing WHEN ringSnapshot is on: the ring's encode reads it instead of the live source.
            // If it won't allocate, the ring cannot run — so treat it exactly like a failed ring build:
            // tear the half-built bus down and fall through to whole-frame. With ringSnapshot OFF the ring
            // reads the live source, so the buffer isn't needed — free it (ringSnapshot triggers a rebuild,
            // so this branch re-runs on the toggle) and skip the alloc, keeping the memory readout honest.
            if (!ringSnapshot) freeSnapshot();
            if (peripheral_->busInitRing(rowBytes, static_cast<uint32_t>(maxLaneLights_))
                && (!ringSnapshot || ensureSnapshotCap())) {
                inited_ = true;
                dmaBuf_ = peripheral_->busBuffer(0);   // ring[0] — a real pointer, the "inited" sentinel
                for (uint8_t i = 0; i < kMaxLanes; i++) busPins_[i] = laneList_[i];
                busLaneCount_ = laneCount_;
                peripheral_->recordBusPins();
                if (status() == peripheral_->initFailMsg()) clearStatus();
                // Re-issue the driving status WITH the ring's regime word — the regime (primed vs
                // lapping) is a platform fact that exists only now, after the ring build; parseConfig
                // set the plain form before it could know. Same numbers, same severity rules.
                // Only when the plain form is what's showing — a clamp warning (or any more urgent
                // status) must keep winning, same rule as parseConfig's `!warn` guard.
                if (const char* mode = peripheral_->busRingMode();
                    mode && status() && std::strncmp(status(), "driving ", 8) == 0) {
                    nrOfLightsType driven = 0;
                    for (uint8_t i = 0; i < laneCount_; i++) driven += laneCounts_[i];
                    if (driven > 0) setDrivingInfo(driven, winLen_, correction_.outChannels, mode);
                }
                return;
            }
            // Ring build failed (the small buffers or the snapshot didn't fit) — drop whatever the ring
            // did allocate, then fall through to whole-frame.
            //
            // busDeinit() DIRECTLY, not deinit(): deinit() only tears the bus down `if (inited_)`, and
            // `inited_` is false here by construction (the deinit() above cleared it; only the success
            // branch sets it). The dangerous case is busInitRing SUCCEEDING and ensureSnapshotCap then
            // failing — a fully-built ring, GDMA channel + ISR + ~150 KB of internal DMA RAM, that
            // deinit() would walk straight past, and the whole-frame busInit below would overwrite the
            // handle of. That leaks the scarcest memory on the chip on exactly the OOM path most likely
            // to hit it, and repeats on every prepare rebuild. busDeinit is idempotent and null-safe.
            peripheral_->busDeinit();
            deinit();
        }

        // WHOLE-FRAME PATH from here — reached when wantsRing() is false (direct mode / useRing off) or the
        // ring build fell through. The whole-frame encode reads the live source directly, so a snapshot left
        // over from a previous ring config is dead weight: free it so the memory readout doesn't report a
        // buffer the current path never touches. (deinit/drain above stopped any refill that read it.)
        freeSnapshot();

        const bool haveSecond = peripheral_->busBuffer(1) != nullptr;
        // Want the second (async) buffer only when the toggle is ON AND the peripheral can run the async
        // path. MoonI80 reports supportsDoubleBuffer()==false — its own-GDMA two-buffer handshake races
        // and wedges the bus — so it always runs single-buffer regardless of the saved toggle (the value
        // is preserved and re-engages on a peripheral that supports it). This is the single "want second?"
        // decision; the reinit-skip check and the busInit request below both read it.
        const bool wantSecond = doubleBuffer && peripheral_->supportsDoubleBuffer();
        if (inited_ && !peripheral_->busIsRing() && peripheral_->busCapacity() == frameBytes_
            && busPinsCurrent() && busLaneCount_ == laneCount_ && haveSecond == wantSecond) {
            // Clear stale latch-pad bytes in BOTH buffers (buffer 1 is null in single-buffer mode).
            std::memset(dmaBuf_, 0, peripheral_->busCapacity());
            if (uint8_t* b1 = peripheral_->busBuffer(1)) std::memset(b1, 0, peripheral_->busCapacity());
            prefillShiftConstantsIfNeeded();   // the zeroing above wiped them
            return;
        }
        deinit();
        // Pre-check the frame against the driver's DMA budget BEFORE attempting the bus init. A
        // whole-frame driver whose DMA can't reach PSRAM (the classic ESP32 i80 = the I2S peripheral,
        // internal-RAM-only) simply cannot allocate a frame larger than its internal DMA block — and on
        // that chip the failing esp_lcd path can BUSY-WAIT to a watchdog reset rather than return an
        // error. So refuse cleanly with a clear, actionable status instead of choking the init. Budget 0
        // (PSRAM-capable chips, or the streaming ring) means "no bound" → always passes. Cold path.
        // REFUSE a bus pin the chip has wired to flash or PSRAM, for the same reason the DMA budget
        // is pre-checked below: routing I/O onto one corrupts the device rather than failing, so
        // what a user sees is a reset with no panic and no coredump, naming nothing. The platform
        // already knows which pins these are (gpioCapability().reserved); this is the one path that
        // was not asking. Data lanes are checked too, since the same corruption follows whichever
        // bus pin lands there.
        //
        // This is a guard, NOT the fix for the classic-ESP32 hang (backlog-light.md): that one is
        // inside esp_lcd_new_i80_bus itself and survives every legal pin choice. COLD PATH.
        {
            const uint16_t* bus = busPinList();
            const uint8_t width = busPinCount();
            // The bus LANES here; the peripheral's own control pins (i80's WR/DC) are checked by
            // validateBusFatal, which already owns that pair and runs on the same cold path.
            for (uint8_t i = 0; i < width && i < kMaxLanes; i++) {
                const uint16_t pin = bus[i];
                if (pin > 48) continue;                       // unset/NC: nothing routed
                const auto cap = platform::gpioCapability(static_cast<uint8_t>(pin));
                if (cap.validGpio && !cap.reserved) continue;
                // A pin the package lacks fails the same silent way a flash pin does (the
                // ESP32-PICO-V3-02 has no GPIO 18/23), so it is refused here for the same reason.
                std::snprintf(statusBuf_, sizeof(statusBuf_),
                              cap.validGpio ? "GPIO %u is wired to flash/PSRAM on this chip - pick another pin"
                                            : "GPIO %u does not exist on this chip package - pick another pin",
                              unsigned(pin));
                setStatus(statusBuf_, Severity::Error);
                deinit();
                return;
            }
        }
        if (const size_t budget = peripheral_->dmaBudgetBytes();
            !frameFitsDmaBudget(frameBytes_, budget)) {
            // deinit() above already cleared the bus and inited_ — just report and bail.
            // Same message the tick path gives, measured against the DECLARED budget (no bus is up
            // yet, so busCapacity() would read 0): the light count the user has to lower, not a KB
            // figure they would have to convert. reinit() cleared overCapReported_ above, so this
            // reports once per geometry rather than once per attempt.
            reportOverCapacity(correction_.outChannels, budget);
            return;
        }
        // Allocate the second buffer only when wanted (see wantSecond above — gated on the toggle AND the
        // peripheral supporting async). OFF costs exactly one DMA buffer, no async overhead.
        inited_ = peripheral_->busInit(frameBytes_, wantSecond);
        dmaBuf_ = inited_ ? peripheral_->busBuffer(0) : nullptr;
        if (inited_) {
            for (uint8_t i = 0; i < kMaxLanes; i++) busPins_[i] = laneList_[i];
            busLaneCount_ = laneCount_;
            peripheral_->recordBusPins();   // i80 also stores WR/DC; Parlio no-op
            prefillShiftConstantsIfNeeded();
        }
        if (!inited_) {
            clearFailBuf();
            setStatus(peripheral_->initFailMsg(), Severity::Error);
        } else if (status() == peripheral_->initFailMsg()) {
            clearStatus();
        }
    }

    void deinit() {
        if (!peripheral_ || peripheral_->lanesAvailable() == 0) return;
        // Free is a use-after-free if a transfer is still reading the buffer — drain first.
        // (reinit already drains before calling here; release()/onCorrectionChanged reach
        // deinit directly, so the guard belongs here too. Idempotent no-op when idle.)
        drainInFlight();
        if (inited_) peripheral_->busDeinit();
        inited_ = false;
        dmaBuf_ = nullptr;
        active_ = 0;              // next init starts on buffer 0
        inFlight_[0] = inFlight_[1] = false;
        busLaneCount_ = 0;
        // Drop the ring snapshot pointer on EVERY teardown: encodeSrc_ is set per-frame by the ring's
        // tickRing, but encodeRows (which reads it) is shared with the whole-frame paths. A stale non-null
        // encodeSrc_ surviving a ring→whole-frame transition would make a whole-frame encode read a freed
        // snapshot. The inited_ guard in tick() already blocks that, but nulling here closes it at the
        // source (the buffer itself frees in freeSnapshot / release; this only drops the dangling view).
        encodeSrc_ = nullptr;
        // NOTE: wire_ is NOT freed here — deinit() runs inside reinit()'s rebuild path (before busInit),
        // and parseConfig (which sized wire_) already ran, so freeing it would strand the encode. It is
        // freed in release() (the true teardown), and grows-only across reinits.
    }

    bool busPinsCurrent() const {
        for (uint8_t i = 0; i < laneCount_; i++)
            if (busPins_[i] != laneList_[i]) return false;
        return peripheral_ && peripheral_->extraBusPinsCurrent();   // i80 also checks WR/DC
    }

    // --- loopback self-test (control-driven; same status shapes as RMT). Builds
    // the REAL frame with the test pattern on lane 0, deinits the live bus, and
    // hands the platform a private TX path + RMT-RX capture that transmits the
    // genuine frame back to back and verifies every bit. ---

    /// INTRUSIVE ride — arm the RX on the live wire and bit-verify (no bus, no transmit of our own). Base
    /// default because it is driver-agnostic: `platform::ws2812LoopbackRide` only opens the RMT-RX, so every
    /// family shares this one path (unlike busLoopback, which each driver overrides to build its own bus).
    platform::RmtLoopbackResult busLoopbackRide(const uint8_t* sent, uint8_t sentLen,
                                                size_t dataBytes, uint8_t rowBits) {
        return platform::ws2812LoopbackRide(static_cast<uint16_t>(loopbackRxPin), sent, sentLen,
                                            dataBytes, rowBits, busClockMultiplier());
    }

    /// INTRUSIVE loopback — verify the LIVE pipeline's wire, building NO bus and freeing NO ring (see the
    /// loopbackIntrusive member doc + platform::ws2812LoopbackRide). Pins the pattern onto the tapped strand
    /// via the snapshot hold, lets the running ring clock a few frames so the hold takes, then arms the RX to
    /// catch one and bit-verify. `lights`/`outCh` come from runLoopbackSelfTest (its cap + channel count).
    void runIntrusiveLoopback(nrOfLightsType lights, uint8_t outCh) {
        if (loopbackRxPin < 0) {
            clearFailBuf();
            setStatus("loopback: set loopbackRxPin (jumper it to the tapped strand)", Severity::Status);
            return;
        }
        // The tapped strand: loopbackStrand in expander mode (any '595 output), else lane 0 (direct).
        const uint8_t strand = pinExpanderMode() && loopbackStrand < laneCount_ ? loopbackStrand
                                                                                : uint8_t{0};
        // dataBytes = the tapped strand's WS2812 byte count = lights × channels × 24 bits (÷ nothing here;
        // the capture derives kBits = dataBytes/3). Width-independent, exactly like the private-bus path.
        const size_t dataBytes = static_cast<size_t>(lights) * outCh * 24;
        // Pin the pattern onto the strand and let the ring pick it up over a few frames (the snapshot hold
        // stamps it every transmit). No deinit, no alloc — the running pipeline keeps rendering.
        patternHoldStrand_ = static_cast<int16_t>(strand);
        platform::delayMs(40);   // a handful of 100 fps frames so the held pattern is on the wire before capture
        const uint8_t pat[3] = {kPatternRGB_[0], kPatternRGB_[1], kPatternRGB_[2]};
        const auto r = busLoopbackRide(pat, outCh < 3 ? outCh : uint8_t{3}, dataBytes,
                                       static_cast<uint8_t>(outCh * 8));
        patternHoldStrand_ = -1;   // release the hold — the strand returns to the live effect next frame
        // Report with the shared verdict formatter (same status strings as the private-bus path).
        reportLoopbackResult(r, outCh);
    }

    void runLoopbackSelfTest() {
        if (!peripheral_ || peripheral_->lanesAvailable() == 0) {
            clearFailBuf();
            setStatus("loopback: not supported on this platform", Severity::Warning);
            return;
        }
        if (laneCount_ == 0) {
            clearFailBuf();
            setStatus("loopback: no valid pins", Severity::Warning);
            return;
        }
        // RX pin must be set — an unset rxPin (-1) has nothing to capture on, and the
        // uint16_t cast at the busLoopback call would turn -1 into a bogus pin.
        if (loopbackRxPin < 0) {
            clearFailBuf();
            setStatus("loopback: set loopbackRxPin (jumper it to the TX pin)", Severity::Status);
            return;
        }
        const uint8_t outCh = correction_.outChannels;
        if (frameBytes_ == 0 || maxLaneLights_ == 0 || outCh == 0) {
            clearFailBuf();
            setStatus("loopback: no lights to encode", Severity::Warning);
            return;
        }
        // Cap the test frame to kLoopbackTestLights (see its declaration for why a big frame
        // overruns the P4 Parlio transfer limit + the RMT-RX capture buffer).
        const nrOfLightsType lights =
            maxLaneLights_ < kLoopbackTestLights ? maxLaneLights_ : kLoopbackTestLights;

        // INTRUSIVE loopback — verify the LIVE pipeline instead of a private replica. This branch NEVER
        // deinits or builds a bus: it pins a known pattern into the running source for the tapped strand,
        // arms the RX on the wire the pipeline is already driving, and bit-verifies. It reaches the ring
        // (a scattered ring shows as a bit fault at a slice boundary) at ZERO extra RAM — the private-bus
        // path below can't, because rebuilding the ring's ~150 KB pool fails on a fragmented heap. Runs the
        // same on every driver: the ride is driver-agnostic (platform::ws2812LoopbackRide arms only the RX).
        if (loopbackIntrusive) { runIntrusiveLoopback(lights, outCh); return; }
        // The loopback frame width is per-driver (kLoopbackFullWidth): the i80 loopback rebuilds
        // the FULL-WIDTH private bus (it can't do a 1-lane bus), so a 16-lane driver's frame must
        // be 16-bit slots to match — and this then genuinely exercises the 16-bit transpose+DMA on
        // real silicon. Parlio builds a 1-lane private unit, so its loopback stays 8-bit regardless.
        const uint8_t sb = peripheral_->loopbackFullWidth() ? slotBytes() : 1;
        // The expander multiplies the SLOT COUNT (a '595 is serial-in: each WS2812 slot is shifted
        // out over 8 bus words), exactly as frameBytesFor does for the operational frame. Omitting it
        // here would build a frame 8× too small and transmit a truncated waveform.
        const uint8_t opp = outputsPerPin();
        const size_t perLightBytes = static_cast<size_t>(outCh) * 8 * 3 * sb * opp;
        // Shift mode closes the frame with one trailing latch word (encodeWs2812ShiftLatchPad, written
        // unconditionally after the last row), so the buffer must hold one Slot beyond the rows or that
        // write runs off the end of the heap block. Direct mode writes no pad, so it stays row-exact.
        const size_t testFrameBytes = static_cast<size_t>(lights) * perLightBytes
                                      + (pinExpanderMode() ? sb : 0);
        // Build the REAL frame with the test pattern in every row on lane 0 only;
        // the platform transmits the genuine transfer (size, DMA chain, latch pad)
        // back to back and verifies every captured bit, so the test covers what
        // the render loop actually sends. Heap alloc is fine: control-driven, off
        // the hot path.
        auto* frame = static_cast<uint8_t*>(platform::alloc(testFrameBytes));
        if (!frame) {
            clearFailBuf();
            setStatus("loopback: out of memory", Severity::Error);
            return;
        }
        std::memset(frame, 0, testFrameBytes);
        // One light's wire bytes, indexed BY STRAND (`wire[strand * outCh + ch]`, the encoder's
        // layout). Direct mode drives lane 0, so byte 0 is enough — but shift mode can put the
        // pattern on ANY strand (loopbackStrand, so the jumper can come off a spare '595 output), and
        // the encoder would then read at `strand * outCh`. Size for the full strand range or that is
        // an out-of-bounds read. Zeroed, so every strand but the chosen one is black.
        const uint8_t patStrand = pinExpanderMode() && loopbackStrand < kMaxStrands ? loopbackStrand
                                                                              : uint8_t{0};
        const size_t wireBytes = static_cast<size_t>(patStrand + 1) * outCh;
        auto* wire = static_cast<uint8_t*>(platform::alloc(wireBytes));
        if (!wire) { platform::free(frame); clearFailBuf(); setStatus("loopback: out of memory", Severity::Error); return; }
        std::memset(wire, 0, wireBytes);
        uint8_t* pat = wire + static_cast<size_t>(patStrand) * outCh;   // the chosen strand's slot
        pat[0] = 0xA5; if (outCh > 1) pat[1] = 0x00; if (outCh > 2) pat[2] = 0xFF;   // R/G/B pattern
        // Encode the pattern on lane 0 at the operational width. A wider bus adds only
        // idle high lanes (activeMask = bit 0); the private bus + loopbackTxPin override
        // carry lane 0's signal to the jumpered RX pin. Sized-per-slot so the 16-bit
        // buffer stride matches the 16-bit private bus.
        //
        // In SHIFT mode the pattern lands on strand 0 — which is data pin 0's '595, shift position 0,
        // i.e. output **Q7** of that register (a '595 shifts MSB-first: the first bit clocked in ends
        // up on the last output). That is the pin to jumper back; see the wiring note on the
        // loopbackRxPin control. Everything else is identical, so the same rig verifies the whole
        // chain through the shift register.
        if (pinExpanderMode()) {
            if (sb == 1) encodeLoopbackFrameShift<uint8_t>(frame, wire, outCh, lights);
            else         encodeLoopbackFrameShift<uint16_t>(frame, wire, outCh, lights);
        } else if (sb == 1) {
            encodeLoopbackFrame<uint8_t>(frame, wire, outCh, lights);
        } else {
            encodeLoopbackFrame<uint16_t>(frame, wire, outCh, lights);
        }
        platform::free(wire);
        // dataBytes drives the RX capture's WS2812-bit count (kBits = dataBytes/3),
        // which is the wire signal on lane 0's RX pin — the SAME bit count at any bus
        // width. So it is width-INDEPENDENT (no ×slotBytes): a wider bus fattens each
        // slot in the DMA buffer (that's testFrameBytes/frameBytes, ×sb) but clocks
        // the same number of WS2812 bits out of lane 0.
        const size_t dataBytes = static_cast<size_t>(lights) * outCh * 24;
        deinit();   // free the live bus; the test builds its own on the data pins
        // TX override: the loopback drives lane 0 only, so when loopbackTxPin is
        // set, transmit on it instead of laneList_[0] (the operational LED pin),
        // letting the test run on a dedicated jumper without re-typing `pins`. The
        // subclass busLoopback reads laneList_, so substitute lane 0 around the call
        // and restore it after, so the following reinit() rebuilds the real bus.
        // The TX override is a DIRECT-mode affordance only. In shift mode the captured signal comes
        // off a '595 output, not off a GPIO the driver can re-point — the strand is determined by
        // which register output the jumper is on, not by which pin transmits. Substituting a pin here
        // would just build the bus on the wrong GPIO, so ignore the override (busPinList() copies
        // laneList_, and the '595s must stay wired to the real data pins for the test to mean
        // anything).
        const uint16_t realLane0 = laneList_[0];
        if (loopbackTxPin >= 0 && !pinExpanderMode()) laneList_[0] = static_cast<uint16_t>(loopbackTxPin);
        const auto r = peripheral_->busLoopback(frame, testFrameBytes, dataBytes,
                                                static_cast<uint8_t>(outCh * 8));
        laneList_[0] = realLane0;
        platform::free(frame);
        // Loopback result first, then reinit: if rebuilding the real bus fails
        // afterwards, kInitFailMsg overwrites the verdict — an unusable driver
        // matters more than a passed self-test.
        reportLoopbackResult(r, outCh);
        reinit();
    }

    /// Turn a loopback result into a status string — shared by the private-bus self-test and the intrusive
    /// ride so the verdict reads identically whatever drove the wire. Names the fault CLASS: no-jumper, PASS,
    /// an empty capture (transport/wiring — reports symbols/idle/tx-time, not a misleading "bad bit"), or a
    /// decoded mismatch (the first bad bit → which light). `outCh` maps firstBadBit to a light (rowBits = ×8).
    void reportLoopbackResult(const platform::RmtLoopbackResult& r, uint8_t outCh) {
        if (!r.jumperDetected) {
            clearFailBuf();
            setStatus("loopback: jumper not detected", Severity::Warning);
        } else if (r.pass) {
            clearFailBuf();
            setStatus("loopback PASS", Severity::Status);
        } else if (failBufEnsure()) {
            if (r.bitsChecked == 0) {
                std::snprintf(failBuf_, kFailBufLen,
                              "no capture: %u sym idle=%d tx %u/%uus",
                              static_cast<unsigned>(r.capturedSymbols),
                              static_cast<int>(r.rxIdleLevel),
                              static_cast<unsigned>(r.txWallUs),
                              static_cast<unsigned>(r.txExpectUs));
            } else {
                const unsigned rowBits = static_cast<unsigned>(outCh) * 8u;
                const unsigned badLight = rowBits ? r.firstBadBit / rowBits : 0u;
                std::snprintf(failBuf_, kFailBufLen,
                              "loopback FAIL: bad bit %u/%u (light %u)",
                              static_cast<unsigned>(r.firstBadBit),
                              static_cast<unsigned>(r.bitsChecked), badLight);
            }
            setStatus(failBuf_, Severity::Error);
        } else {
            setStatus("loopback FAIL", Severity::Error);
        }
    }
};

} // namespace mm
