#pragma once

#include "light/drivers/DriverBase.h"

#include "light/drivers/LedDriverConfig.h"
#include "light/drivers/PinList.h"         // parsePinList / assignCounts (shared with MultiPinLedDriver)
#include "light/drivers/RmtSymbol.h"       // makeRmtSymbol (the bit shapes the peripheral expands with)
#include "platform/platform.h"

namespace mm {

/// Output driver: WS2812B-class addressable LEDs over the ESP32 [RMT (Remote Control
/// Transceiver)](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/peripherals/rmt.html)
/// peripheral — one GPIO and one RMT TX channel per strand, fed consecutive slices of the source
/// buffer (8-bit, GRB). The default LED driver for classic-ESP32 and S3 board entries, and the
/// readable EXAMPLE future LED drivers copy: a sibling of NetworkSendDriver (same DriverBase hooks,
/// same per-light `correction_.apply()` guard, same once-allocated owned buffer sized off the hot
/// path); only the emit differs — this fuses the correction + WS2812 symbol-encode into one pass
/// (the encode is `RmtSymbol.h`, host-tested) then hands per-pin slices to the platform.
///
/// **Wire contract — [WS2812B](https://cdn-shop.adafruit.com/datasheets/WS2812B.pdf):** 1-wire NRZ
/// at 800 kHz, no clock line. Each bit is a 1.25 µs cell that starts HIGH then drops LOW; the HIGH
/// duration encodes the bit (`0` = 350 ns high, `1` = 700 ns high), MSB-first per byte. Channel
/// order (GRB, GRBW, …) is applied by `Correction` before the encode, so the encoder is
/// order-agnostic. Frames latch on ≥300 µs idle-LOW. Timings live in `LedDriverConfig`, converted to
/// RMT ticks from the granted resolution (~40 MHz), never hard-coded to one clock. The platform owns
/// only the peripheral (`platform::rmtWs2812*`); on a chip without RMT TX channels those calls are
/// inert stubs, so it compiles everywhere. The peripheral half uses the modern RMT driver (ESP-IDF
/// 5.x "RMT v2": `rmt_new_tx_channel` / a copy encoder / `rmt_transmit`), not the legacy
/// channel-numbered API — not a preference: the legacy driver was removed entirely in ESP-IDF v6
/// (the build IDF), so v2 is the only API that exists. On chips whose RMT has a DMA backend
/// (`SOC_RMT_SUPPORT_DMA` — the P4; the classic ESP32 has none) the whole-frame loopback capture
/// uses it.
///
/// **Flicker on LEDs that should be off** is almost always a data-line signal-integrity problem
/// (3.3 V drive into a 5 V strip), not firmware — the "LED signal integrity" use-case guide has the
/// confirm-firmware-innocent playbook (`loopbackFrame`, the TX-power sweep) and the electrical fixes.
/// @card RmtLedDriver.png
class RmtLedDriver : public DriverBase {
public:
    /// WS2812/SK6812 strips are physically GRB-wired, so a fresh RMT driver references the "GRB"
    /// preset by default (a strip attached to a freshly-flashed board shows correct colors). The
    /// user can pick any preset from the library.
    RmtLedDriver() { setDefaultPresetName("GRB"); }

    /// Hard cap on the pin arrays: the largest RMT TX group of any supported chip (8 on
    /// classic ESP32; the S3 has 4 — enforced per target via maxPinsForTarget()). A fixed
    /// array bounded by a hardware constant, not a dynamic list: the bound can't grow at runtime.
    static constexpr uint8_t kMaxPins = 8;

    /// Comma-separated GPIO list, one RMT TX channel per pin ("18,17,16"). Text control so one
    /// field holds N pins — per-output (pin, count) rows are the WLED LED-settings pattern. The
    /// peripheral validates each pin at init; a parse error or failing pin lands in the status
    /// field and the driver idles. 24 bytes fit kMaxPins 2-digit GPIOs plus separators. Defaults
    /// to UNSET: the strand is user-soldered to whatever GPIO the user wired, so a hard-coded pin
    /// would be a guess that could drive a pin committed elsewhere — empty until set, idle
    /// meanwhile (the "default only when it cannot do harm" rule; see lessons.md). Bench pin "18".
    char pins[24] = "";

    /// Comma-separated lights-per-pin ("100,100,50"), matched to `pins` by position — each pin
    /// takes the next consecutive slice of the source buffer, in list order (pin 1 = `[0,n₁)`,
    /// pin 2 = `[n₁,n₁+n₂)`, …). May be empty or shorter than `pins`: the unassigned remainder
    /// splits evenly over the remaining pins (last takes the rounding remainder), so the empty
    /// default splits the whole buffer evenly. Each pin is capped at a WS2812 per-pin ceiling
    /// (2048 lights — a 1-wire line clocks ~30 µs/light, so 2048 is already ~16 FPS): a pin over
    /// the ceiling is clamped (output stays lit) with a Warning status, guarding the common
    /// misconfig of a whole grid on one pin. Use the start/count window to drive fewer lights,
    /// not this safety cap.
    char ledsPerPin[48] = "";

    /// Wire timing. Named by SPEED rather than by chipset, because chipset names do not partition
    /// the timings: SK6812 and WS2812B decode identically, while "WS2811" covers two different bit
    /// rates. The chip names in each label are examples of what that timing drives, not a list.
    ///
    /// The default satisfies WS2812, WS2812B and SK6812 at once, which is why it has served every
    /// strip so far. A 12V WS2811 strip in its low-speed mode needs twice the bit cell and reads
    /// the default as noise past the first few lights (issue #94).
    uint8_t timing = 0;

    /// The three numbers behind `timing`, shown only when it is `custom`. A strip whose datasheet
    /// matches no preset is then a control change rather than a firmware release, and what a user
    /// finds by trying is a number they can report back.
    uint16_t t0hNs = 350;
    uint16_t t1hNs = 700;
    uint16_t periodNs = 1250;

    /// The index of `custom` in the table below: the one option that reads the three fields.
    static constexpr uint8_t kTimingCustom = 3;

    /// The presets, in the order the select lists them.
    static constexpr const char* kTimingOptions[] = {
        "800kHz WS2812B/SK6812",   // 350 / 700 / 1250: the default, and most strips
        "400kHz WS2811",           // 500 / 1200 / 2500: 12V WS2811 in low-speed mode
        "800kHz WS2811 fast",      // 250 / 600 / 1250: WS2811 strapped to high speed
        "custom",                  // the three fields below
    };

    /// On-device loopback self-test — RMT is a transceiver, so the driver verifies its own output
    /// on real silicon (replaces the old standalone test firmware). Tick to run a one-shot RMT
    /// TX→RX round-trip: jumper the first pin (TX) to `loopbackRxPin`, it transmits a known WS2812
    /// pattern, captures it back, decodes, compares → `loopback PASS` / `FAIL: …` / `jumper not
    /// detected` in the status field. A persistent on/off mode (see onControlChanged): while on the test
    /// re-runs on every relevant change; turning it off clears the verdict. Hardware lives in
    /// `platform::rmtWs2812Loopback*`.
    bool     loopbackTest = false;  // checkbox: on = run + keep re-running on change
    /// Optional TX override for the test: when set (>= 0), the loopback transmits on THIS pin in
    /// place of pins[0], so the test can run on a dedicated jumper without re-typing the
    /// operational `pins`. Falls back to pins[0] when unset (-1). Test-only — normal output uses
    /// `pins`. int8_t + addPin (not uint16): single-GPIO controls use the standard Pin control,
    /// and -1 = unset lets GPIO 0 be a valid loopback pin (0-as-unset wouldn't).
    int8_t   loopbackTxPin = -1;
    /// Jumper this to the TX pin for the test (unset = -1 by default; bench used pin 5).
    int8_t   loopbackRxPin = -1;

    /// Whole-frame stress variant: instead of a 24-bit burst, transmit a real frame the size of
    /// the first pin's slice, back to back, and bit-verify the WHOLE capture. This is the one that
    /// catches frame-rate corruption and RF interference on the data line (the flicker class of
    /// bug) — a 24-bit burst passes through a wire that mangles a sustained frame. Shown only in
    /// test mode; the status names the first corrupted light on failure. On the classic ESP32,
    /// which has no RMT DMA, the capture is capped to one channel's worth of symbols (~2 RGB lights)
    /// and still clocked back to back; the S3/P4 capture the full frame via DMA.
    bool     loopbackFrame = false;

    // 40 MHz RMT tick clock = 25 ns/tick: t0h 350ns→14, t1h 700ns→28, period
    // 1250ns→50 ticks. The encoder converts ns→ticks via the granted resolution,
    // so this is the requested clock, not a hard-coded tick count.
    static constexpr uint32_t kResolutionHz = 40'000'000;

    // The pin/count list parsing (parsePinList / assignCounts) lives in
    // PinList.h, shared with MultiPinLedDriver — both drivers slice the source
    // buffer from the same two text controls.

    /// Bind the driver's controls: the window (start/count), the `pins` and
    /// `ledsPerPin` text lists, and the loopback self-test controls (the TX/RX pin
    /// overrides and frame-stress flag are always bound but shown only in test mode).
    void defineDriverControls() override {
        addWindowControls();   // start / count — the slice of the shared buffer this driver outputs
        controls_.addText("pins", pins, sizeof(pins));
        controls_.addText("ledsPerPin", ledsPerPin, sizeof(ledsPerPin));
        controls_.addSelect("timing", timing, kTimingOptions,
                            static_cast<uint8_t>(sizeof(kTimingOptions) / sizeof(kTimingOptions[0])));
        // The custom fields are always bound so persistence can load them whatever the mode, and
        // shown only in custom mode: the same add-then-setHidden shape the loopback controls use.
        // Ranges are the encodable window at the 40 MHz tick clock, not datasheet limits: a user
        // reading their own datasheet should be able to type what it says.
        const bool custom = timing == kTimingCustom;
        controls_.addControl("t0hNs", t0hNs, 50, 4000);
        controls_.setHidden(controls_.count() - 1, !custom);
        controls_.addControl("t1hNs", t1hNs, 50, 4000);
        controls_.setHidden(controls_.count() - 1, !custom);
        controls_.addControl("periodNs", periodNs, 200, 8000);
        controls_.setHidden(controls_.count() - 1, !custom);
        controls_.addControl("loopbackTest", loopbackTest);
        controls_.setAdvanced(controls_.count() - 1);   // expert-mode: a bench self-test, not a normal-use control
        // loopbackTxPin / loopbackRxPin are always bound (so persistence can load
        // them any time) but only shown while the test mode is on — same always-
        // add-then-setHidden shape NetworkModule uses for its static-IP fields. The
        // rebuild after every control change (HttpServerModule) re-runs this and
        // flips the flag. txPin is the optional override: -1 (unset) = transmit on
        // pins[0]; a value >= 0 is an explicit GPIO override (0 is now a valid pin).
        controls_.addPin("loopbackTxPin", loopbackTxPin);
        controls_.setHidden(controls_.count() - 1, !loopbackTest);
        controls_.addPin("loopbackRxPin", loopbackRxPin);
        controls_.setHidden(controls_.count() - 1, !loopbackTest);
        controls_.addControl("loopbackFrame", loopbackFrame);
        controls_.setHidden(controls_.count() - 1, !loopbackTest);
    }

    /// Changing the pin list or the per-pin counts re-parses and re-inits the RMT
    /// channels (live, not reboot-to-apply), so the pipeline-wide prepare
    /// sweep runs and parseConfig()/reinit() pick up the new lists.
    bool affectsPrepare(const char* name) const override {
        return std::strcmp(name, "pins") == 0 || std::strcmp(name, "ledsPerPin") == 0
            || std::strcmp(name, "timing") == 0 || std::strcmp(name, "t0hNs") == 0
            || std::strcmp(name, "t1hNs") == 0 || std::strcmp(name, "periodNs") == 0
            // Not because it rebuilds anything: defineControls decides whether the loopback pins
            // are shown, and only a prepare sweep re-runs it. Without this the checkbox toggles a
            // mode whose three controls never appear, so the test cannot be aimed from the UI.
            || std::strcmp(name, "loopbackTest") == 0
            || isWindowControl(name);
    }

    /// React to a control change (runs off the render loop, in the HTTP/API
    /// handler context — a blocking self-test here is fine). loopbackTest is a
    /// persistent on/off mode. While it's ON, the test (re-)runs on every relevant
    /// change — turning it on, OR editing pins / loopbackRxPin — so the pins can be
    /// set in any order and the result always reflects the current pins. Turning it
    /// OFF clears the result.
    void onControlChanged(const char* name) override {
        const bool isTestControl = std::strcmp(name, "loopbackTest") == 0;
        const bool isPinControl  = std::strcmp(name, "pins") == 0
                                || std::strcmp(name, "loopbackTxPin") == 0
                                || std::strcmp(name, "loopbackRxPin") == 0
                                || std::strcmp(name, "loopbackFrame") == 0;
        if (isTestControl && !loopbackTest) {
            // Toggling the test off clears the loopback verdict, then re-derives
            // the real driver status — a config/init error must survive (a blind
            // clearStatus() would hide it).
            clearFailBuf();
            clearStatus();
            parseConfig();
            reinit();
        } else if (loopbackTest && (isTestControl || isPinControl)) {
            // A `pins` edit changes pinList_/pinCount_, but onControlChanged runs BEFORE the
            // prepare() sweep re-parses (and loopbackRxPin/loopbackFrame don't
            // trigger that sweep at all), so refresh here before testing — otherwise
            // the self-test would transmit on the STALE pinList_[0] and show a verdict
            // for the previous pin. Mirrors ParallelLedDriver::onControlChanged.
            if (std::strcmp(name, "pins") == 0) { parseConfig(); reinit(); }
            runLoopbackSelfTest();
        }
        // Chain to the base so a correction-control edit (localBrightness / preset / whiteMode)
        // rebuilds this driver's correction LUT — without this the LED driver's brightness/preset
        // controls were dead (only the global-brightness push reached the LUT).
        DriverBase::onControlChanged(name);
    }

    /// Parse the config and (re)init the RMT channels. Lifecycle has two
    /// deliberately-separate concerns, so the buffer half stays host-testable and a
    /// hardware-only guard can never strand it:
    ///   - SYMBOL BUFFER (plain heap): resizeFrame() / freeFrame(), run on
    ///     every platform.
    ///   - RMT CHANNELS (hardware): reinit() / deinitAll(), RMT-targets-only
    ///     (if constexpr).
    /// The original bug put the buffer free inside the hardware deinit(), which
    /// reinit() (a rebuild) calls — so a rebuild freed the buffer tick() needs.
    /// Keeping the two apart makes that mistake impossible here and lets the host
    /// unit test (unit_RmtLedDriver_lifecycle.cpp) pin it.
    /// One-time wiring only (parse the pin lists into members); the RMT/buffer acquire lives
    /// in prepare(), the sole resource-lifecycle gate. Enabled-independent — the acquire
    /// happens in the prepareTree sweep that always follows.
    void setup() override { parseConfig(); }
    /// Release the RMT channels and free the symbol buffer, then clear the shared
    /// fail/config-error state (DriverBase::release()).
    void release() override {
        deinitAll();
        freeFrame();
        DriverBase::release();   // frees the correction scratch, clears failBuf_ + configErr_
    }

    /// Pure build (see MoonModule::prepare): re-parse, resize the symbol buffer, and (re)init the
    /// RMT channels off the hot path (tick() never allocates). No enabled() check — core's applyState()
    /// only calls this when effectively-enabled and routes to release() (release) otherwise, so the
    /// channels + buffer free when the driver, or a parent, is disabled.
    void prepare() override {
        // Drain first. resizeFrame() may free the symbol buffer and reinit() deletes the
        // channel, and a prepare arrives from a control change, which can land mid-frame: the
        // peripheral is then still reading those symbols. Bounded, because a wedged transfer must
        // not block a config change forever; past the deadline the rebuild proceeds, which is the
        // pre-existing behavior rather than a new risk.
        if (txInFlight_) {
            for (uint8_t attempt = 0; attempt < 4 && txInFlight_; attempt++)
                txInFlight_ = !waitForPins();
            // Still busy after every attempt: the peripheral is reading frame_ right now, so
            // rebuilding would free the buffer under it, which is the corruption this drain exists
            // to prevent. Defer instead. tick() re-waits and the config applies on a later prepare;
            // the alternative, rebuilding anyway, trades a delayed config change for a torn frame.
            if (txInFlight_) return;
        }
        parseConfig();
        resizeFrame();
        reinit();
        // Re-assert the resting "driving N of M lights" status after the full build. parseConfig sets it
        // too, but only when a buffer is already wired (txLightCount_ > 0); on the boot path setup()'s
        // parseConfig runs before the source buffer exists, so it's skipped and the status stays blank
        // (or shows a stale loopback verdict) until the user touches a control. Re-deriving here — once
        // pins + buffer + counts are all settled — makes it the default resting state, the way MoonLed's
        // shows. Gated on inited_: reinit() reports a per-pin "RMT init failed" at Severity::Error without
        // touching configErr_/configWarn_, so the `!warn` rule alone would overwrite that error with a
        // false "driving N lights" while tick() bails and the strand stays dark. Only assert the resting
        // status when the channels actually came up.
        // frameUnusable_ joins inited_ here for the same reason: resizeFrame reports the
        // no-transmit case at Severity::Error without touching configErr_/configWarn_, so the
        // `!warn` rule alone would replace it with a false "driving N lights" while the strip sits
        // frozen. That false-OK status is precisely what made issue #94 so hard to place.
        if (inited_ && !frameUnusable_ && !configErr_ && !configWarn_ && txLightCount_ > 0)
            setDrivingInfo(txLightCount_, winLen_, correction_.outChannels);
    }

    /// Preset toggle (RGB↔RGBW) changes outChannels without a structural rebuild —
    /// the per-pin symbol offsets scale with outChannels, so re-derive them too. Skipped
    /// while (effectively) disabled (would re-alloc the symbol buffer a disabled driver released).
    void onCorrectionChanged() override { if (!effectivelyEnabled()) return; parseConfig(); resizeFrame(); }

    /// Point the driver at the source frame buffer; re-parse (counts derive from its light count)
    /// and resize the symbol buffer to match. The resize is skipped while (effectively) disabled
    /// (a disabled driver holds no buffer); the enabled prepare() sweep re-sizes it on enable.
    void setSourceBuffer(Buffer* buf) override {
        sourceBuffer_ = buf;
        parseConfig();      // counts derive from the buffer's light count
        if (effectivelyEnabled()) resizeFrame();
    }

    /// Per-tick output: fuse the correction and WS2812 symbol-encode in one pass
    /// over this driver's window, then start every pin's transmit before waiting on
    /// any, so the tick costs the longest strand rather than the sum. Inert off RMT
    /// chips and idle until inited with a source buffer + correction.
    void tick() MM_NONBLOCKING override {
        if constexpr (platform::rmtTxChannels == 0) return;  // inert off RMT chips
        if (!inited_ || !sourceBuffer_ || !sourceBuffer_->data()) return;

        // Encode only the lights the pins actually transmit (Σ pinCounts_), NOT the whole source
        // buffer: a strand config of e.g. 64 leds/pin on a 16K-light grid drives 64, so encoding
        // all 16384 would burn ~100× the work the output needs (the rest is never clocked out).
        // Bounded by the buffer too, in case config outruns the current frame.
        // Encode within this driver's window only. winLen_ is the slice length;
        // txLightCount_ (Σ pinCounts_) is what the pins clock out — n is the min,
        // so a window smaller than the configured pin total never reads past it.
        // A frame still on the wire OWNS frame_: the peripheral expands its bytes straight out of it,
        // so re-encoding now rewrites bytes the peripheral is mid-way through clocking. That is not
        // a dropped frame, it is a corrupted one, and it shows as a handful of lights in a color
        // the effect never drew. Only a timed-out wait leaves this set, so the normal path never
        // sees it; when it happens, skipping the tick lets the transfer finish and the next tick
        // encodes cleanly. Bench: this is what remained after the memory-block and interrupt
        // priority work, and it is independent of light count, which is what ruled those out.
        if (txInFlight_) {
            txInFlight_ = !waitForPins();     // still busy: leave frame_ alone for another tick
            if (txInFlight_) return;
        }

        const nrOfLightsType n = txLightCount_ < winLen_ ? txLightCount_ : winLen_;
        const uint8_t outCh = correction_.outChannels;
        // Same defensive guard ArtNet uses: skip rather than overrun if the
        // symbol buffer is stale (e.g. correction swapped without a resize).
        if (n == 0 || outCh == 0 || pinCount_ == 0
            || !frame_ || frameCap_ < frameBytesFor(n, outCh)) return;   // buffer not ready

        // Fused single pass: correct one light into wire bytes, encode those
        // bytes straight into the symbol buffer. No second sweep over encoded
        // data, no per-light heap.
        const uint8_t* src = sourceBuffer_->data();
        const uint8_t srcCh = sourceBuffer_->channelsPerLight();
        // Correct each light straight into the WIRE-BYTE frame. The bit expansion that used to
        // happen here (one 32-bit symbol per data bit, 96 bytes per RGB light) now happens on the
        // way to the peripheral, so this buffer is outCh bytes per light: 3 KB at 1024 lights
        // where the symbol form wanted 96 KB and fell back to unusable PSRAM (issue #94).
        for (nrOfLightsType i = 0; i < n; i++) {
            correction_.apply(src + (winStart_ + i) * srcCh, frame_ + static_cast<size_t>(i) * outCh, srcCh);
        }
        // Start every pin's slice before waiting on any — the channels clock out
        // concurrently, so the tick is charged the longest strand, not the sum.
        // The shared reset gap (the WS2812 latch) runs once, after the last wait.
        // Wait ONLY on channels whose transmit started: a failed transmit gives
        // no done-callback, so waiting on it would block the full 1000 ms timeout
        // and a single bad pin would stall the tick (the same guard the LCD /
        // Parlio loops use, here per channel).
        // Transmit only up to the n lights actually encoded this frame: pins are laid out
        // contiguously from light 0, so pin i covers lights [pinStart, pinStart+pinCounts_[i]).
        // Normally Σ pinCounts_ == n, but if the buffer shrank since the last parseConfig (a grid
        // resize lands a tick before the config re-parse) n can be below Σ pinCounts_ — cap each
        // pin at the encoded boundary so it never clocks out stale symbols past what we wrote.
        const size_t bytesPerLight = static_cast<size_t>(outCh);
        bool started[kMaxPins] = {};
        for (uint8_t i = 0; i < pinCount_; i++) {
            const nrOfLightsType pinStart = static_cast<nrOfLightsType>(pinOffsets_[i] / bytesPerLight);
            if (pinStart >= n) break;  // contiguous: this pin and all later ones are past the encoded lights
            const nrOfLightsType pinLights =
                (pinStart + pinCounts_[i] > n) ? static_cast<nrOfLightsType>(n - pinStart) : pinCounts_[i];
            if (pinLights == 0) continue;
            started[i] = platform::rmtWs2812Transmit(rmt_[i], frame_ + pinOffsets_[i],
                                        static_cast<size_t>(pinLights) * bytesPerLight);
        }
        for (uint8_t i = 0; i < pinCount_; i++) started_[i] = started[i];
        txInFlight_ = !waitForPins();
        if (cfg_.reset_us) platform::delayUs(cfg_.reset_us);
    }

    /// Wait on every pin that actually started, and report whether they all finished. A pin whose
    /// transmit never started is not waited on: with no done-callback coming, that would spend the
    /// full timeout and let one bad pin stall the tick.
    bool waitForPins() MM_NONBLOCKING {
        bool allDone = true;
        for (uint8_t i = 0; i < pinCount_; i++) {
            if (!started_[i]) continue;
            if (platform::rmtWs2812Wait(rmt_[i], 1000 /* ms */)) started_[i] = false;
            else allDone = false;
        }
        return allDone;
    }

    /// Test-only accessors. frameBuffer/frameCapacity mirror ArtNet's
    /// correctedBuffer() and let unit tests pin the buffer-lifecycle invariants a
    /// hardware bug already taught us; pinCount/pinLightCount/pinFrameOffsetBytes
    /// pin the multi-pin slice arithmetic (unit_RmtLedDriver_pins.cpp). Not part
    /// of any runtime API.
    const uint8_t* frameBuffer() const { return frame_; }
    /// Words allocated in the symbol buffer. Test-only.
    size_t frameCapacity() const { return frameCap_; }
    /// Number of parsed output pins (0 = idle). Test-only.
    uint8_t pinCount() const { return pinCount_; }
    /// Lights on pin `i` (0 if out of range). Test-only.
    nrOfLightsType pinLightCount(uint8_t i) const { return i < pinCount_ ? pinCounts_[i] : 0; }
    /// Word offset of pin `i`'s slice in the symbol buffer (0 if out of range). Test-only.
    size_t pinFrameOffsetBytes(uint8_t i) const { return i < pinCount_ ? pinOffsets_[i] : 0; }

private:
    // Source frame. The output correction (channel order + white + brightness) lives on
    // DriverBase, applied per-light via correction_.apply(); same shape as NetworkSendDriver.
    Buffer* sourceBuffer_ = nullptr;

    LedDriverConfig cfg_;

public:
    /// The wire timing the `timing` control resolved to. The tests assert against this rather than
    /// against RMT ticks, so what is pinned is the contract a datasheet states (nanoseconds) rather
    /// than the tick clock that happens to express it. Same injection-point role as
    /// `correctionForTest()`; read-only, since the control is the way to change it.
    const LedDriverConfig& wireTimingForTest() const { return cfg_; }

private:
    platform::RmtWs2812Handle rmt_[kMaxPins];
    uint16_t       pinList_[kMaxPins] = {};    // parsed pins, list order
    nrOfLightsType pinCounts_[kMaxPins] = {};  // lights per pin (slice lengths)
    size_t         pinOffsets_[kMaxPins] = {}; // slice start in frame_, bytes
    nrOfLightsType txLightCount_ = 0;          // Σ pinCounts_ — lights actually transmitted/encoded
    nrOfLightsType winStart_ = 0;              // first source-buffer light this driver reads (the window)
    nrOfLightsType winLen_ = 0;                // window length (lights), clamped to the buffer
    uint8_t pinCount_ = 0;                     // 0 = idle (parse error / no pins)
    bool inited_ = false;                      // all-or-nothing across the pins
    bool started_[kMaxPins] = {};              // which pins have a transmit still to be waited on
    bool frameUnusable_ = false;  // frame buffer missing, or in PSRAM the refill cannot read
    bool txInFlight_ = false;                  // a frame is still clocking out of frame_
    uint8_t* frame_ = nullptr;      // owned; the wire bytes for the whole frame (outChannels per light)
    size_t frameCap_ = 0;          // bytes allocated

    // The parse-error literal currently shown in the status slot (nullptr when
    // configErr_, failBuf_, kFailBufLen and the clearConfigErr/clearFailBuf/
    // failBufEnsure/setConfigErr helpers live on DriverBase (shared verbatim with
    // the LCD and Parlio drivers). The on-demand FAIL string (failBuf_) is only
    // formatted when a loopback or channel init FAILs; PASS/jumper/unsupported use
    // flash literals.

    // The chip's TX-channel cap caps the pin list; on targets without RMT
    // (desktop, where the constant is 0) fall back to kMaxPins so the parsing
    // and slicing stay fully host-testable.
    static constexpr uint8_t maxPinsForTarget() {
        return (platform::rmtTxChannels > 0 && platform::rmtTxChannels < kMaxPins)
                   ? platform::rmtTxChannels
                   : kMaxPins;
    }

    static size_t frameBytesFor(nrOfLightsType lights, uint8_t channels) {
        return static_cast<size_t>(lights) * channels;
    }

    // Convert a ns duration to RMT ticks using the resolution the platform
    // granted. Falls back to the requested clock when not inited (host/desktop).
    /// Hand the peripheral the two symbols a data bit expands to. The bit expansion now happens on
    /// the way out (the IDF bytes encoder, or the level-5 refill), so these shapes are the whole of
    /// what the wire timing means: the `timing` control changes them live, between frames.
    void pushBitTiming(uint8_t i) {
        const uint16_t t0h = nsToTicks(cfg_.t0h_ns);
        const uint16_t t1h = nsToTicks(cfg_.t1h_ns);
        const uint16_t period = nsToTicks(cfg_.period_ns);
        platform::rmtWs2812SetBitTiming(rmt_[i],
            makeRmtSymbol(t0h, 1, static_cast<uint16_t>(period - t0h), 0),
            makeRmtSymbol(t1h, 1, static_cast<uint16_t>(period - t1h), 0));
    }

    uint16_t nsToTicks(uint32_t ns) const MM_NONBLOCKING {
        uint32_t hz = inited_ ? platform::rmtWs2812Resolution(rmt_[0]) : kResolutionHz;
        if (hz == 0) hz = kResolutionHz;
        return static_cast<uint16_t>((static_cast<uint64_t>(ns) * hz) / 1'000'000'000ull);
    }

    // --- pin/count config (plain parsing; runs on every platform) ---

    // Re-derive pinList_/pinCounts_/pinOffsets_ from the two text controls and
    // the current buffer/correction. On error: pinCount_ = 0 (tick() idles) and
    // the static error literal goes to the status slot; a later successful parse
    // clears it. Off the hot path.
    /// Turn the `timing` selection into the wire timing the encoder reads. Called from
    /// parseConfig, so every path that rebuilds the driver picks it up: the numbers are read per
    /// frame from cfg_, so a change takes effect on the next frame with no channel reinit (the RMT
    /// tick clock is unchanged, only how many ticks each bit lasts).
    void applyTiming() {
        switch (timing) {
            // The 400 kHz WS2811 mode: T0H 0.5 us, T1H 1.2 us, 2.5 us cell. Twice the bit period,
            // so a frame takes twice as long; at 18 lights that is ~1 ms, at 1000 it is ~60 ms.
            case 1: cfg_.t0h_ns = 500; cfg_.t1h_ns = 1200; cfg_.period_ns = 2500; break;
            // WS2811 strapped to its high-speed mode: the same 1.25 us cell as WS2812 with narrower
            // pulses. Within the WS2812 decode window, so it is a refinement rather than a rescue.
            case 2: cfg_.t0h_ns = 250; cfg_.t1h_ns = 600;  cfg_.period_ns = 1250; break;
            case kTimingCustom:
                // Trusted as typed, but ordered: a t1h below t0h encodes 1 as a shorter pulse than
                // 0, which no chip decodes, and a period below t1h cannot contain the pulse.
                cfg_.t0h_ns = t0hNs;
                cfg_.t1h_ns = t1hNs > t0hNs ? t1hNs : static_cast<uint16_t>(t0hNs + 50);
                cfg_.period_ns = periodNs > cfg_.t1h_ns ? periodNs
                                                        : static_cast<uint16_t>(cfg_.t1h_ns + 50);
                break;
            // The default: satisfies WS2812, WS2812B and SK6812 at once.
            default: cfg_.t0h_ns = 350; cfg_.t1h_ns = 700; cfg_.period_ns = 1250; break;
        }
    }

    bool parseConfig() {
        applyTiming();
        pinCount_ = 0;
        uint8_t n = 0;
        const char* warn = nullptr;
        const char* err = parsePinList(pins, pinList_, maxPinsForTarget(), n);
        if (!err) {
            // Distribute over the driver's window slice, not the whole buffer, so
            // ledsPerPin's "rest" only fills this driver's [start, start+count).
            // assignCounts clamps each pin to kMaxWs2812LedsPerPin (the driver drives
            // that many rather than choking a whole grid onto one WS2812 line).
            const nrOfLightsType bufN = sourceBuffer_ ? sourceBuffer_->count() : 0;
            windowSlice(bufN, winStart_, winLen_);
            err = assignCounts(ledsPerPin, n, winLen_, pinCounts_, kMaxWs2812LedsPerPin, &warn);
        }
        if (err) {
            setConfigErr(err);
            return false;
        }
        pinCount_ = n;
        const uint8_t outCh = correction_.outChannels;
        size_t off = 0;
        txLightCount_ = 0;
        for (uint8_t i = 0; i < pinCount_; i++) {
            pinOffsets_[i] = off;
            off += static_cast<size_t>(pinCounts_[i]) * outCh;   // BYTES: frame_ holds wire bytes
            txLightCount_ = static_cast<nrOfLightsType>(txLightCount_ + pinCounts_[i]);
        }
        clearConfigErr();
        // assignCounts sets `warn` when it clamped a pin to the WS2812 ceiling — the output
        // still runs (the first 2048/pin), so it's a Warning, not an idling error. Passing it
        // (or null when nothing clamped) to setConfigWarn tracks the live state and retracts a
        // stale warning once the user drops back under the ceiling.
        setConfigWarn(warn);
        // With nothing more urgent to show AND lights actually driven, report the lights
        // this driver consumes (Σ pinCounts_) of the window — real consumption, not a
        // grid×pins guess. An idle driver (no pins) stays statusless, not "driving 0 of 0".
        if (!warn && txLightCount_ > 0) setDrivingInfo(txLightCount_, winLen_, outCh);
        return true;
    }

    // --- symbol buffer (plain heap; runs on every platform) ---

    // (Re)allocate the symbol buffer for the current source + correction. Off the
    // hot path. Grows only — keeps a big-enough existing allocation.
    void resizeFrame() {
        if (!sourceBuffer_) return;
        // Size for the lights this driver actually CLOCKS OUT, not the whole window. The window (start,
        // count) can be far larger than the pins encode: `ledsPerPin` (or fewer pins than the window has
        // lights) caps the transmitted total at `txLightCount_` (Σ pinCounts_), and tick() only ever
        // encodes that many (n = min(txLightCount_, winLen_) there). Sizing to the window instead made an
        // 8×8 strip on one pin (ledsPerPin 64) inside a 70×82 grid (count=all, window 5740) try to alloc
        // ~550 KB of symbols for lights it never encodes — the alloc failed on a small-heap classic ESP32,
        // frame_ stayed null, and tick() bailed, so the strip went dark even though only 64 lights were
        // wanted. Bound to txLightCount_ so the buffer matches the real output. Fall back to the window
        // when no pins are parsed yet (txLightCount_ == 0), so the buffer is ready before pins are set.
        nrOfLightsType winStart, win;
        windowSlice(sourceBuffer_->count(), winStart, win);
        nrOfLightsType n = txLightCount_ > 0 ? txLightCount_ : win;
        if (n > win) n = win;               // never exceed the window's own light count
        const uint8_t ch = correction_.outChannels;
        if (n == 0 || ch == 0) { frameUnusable_ = false; return; }
        // Per-light correction scratch: grow to `ch` bytes when the channel count grows (off the hot
        // path). Sized to outChannels so a >4-channel correction (RGBCCT, a fixture) can't overflow it.
        const size_t need = frameBytesFor(n, ch);
        if (frame_ && frameCap_ >= need) { frameUnusable_ = false; return; }
        freeFrame();
        // INTERNAL RAM, deliberately. On the classic ESP32 the level-5 refill expands these bytes
        // with the flash cache possibly off, where a PSRAM read is a fault rather than a stall; on
        // the DMA chips the bytes encoder reads them under the same deadline pressure. At outCh
        // bytes per light this is ~3 KB for 1024 RGB lights, so internal RAM affords it at any
        // strand length a classic ESP32 can drive. (The pre-expanded symbol form this replaced
        // wanted 96 bytes per light: 96 KB at 1024 lights, which internal RAM does NOT have, so it
        // fell back to PSRAM and the transmit refused every frame. Issue #94.)
        frame_ = static_cast<uint8_t*>(platform::allocInternal(need));
        if (!frame_) frame_ = static_cast<uint8_t*>(platform::alloc(need));
        frameCap_ = frame_ ? need : 0;
        // A failed allocation still has to SAY so: tick()'s `!frame_` guard then skips the frame and
        // the strip would otherwise sit frozen while the card reports "driving N of N" at a healthy
        // fps, which is what made issue #94 so hard to place.
        // The classic-ESP32 refill reads these bytes with the flash cache possibly off, so a
        // PSRAM buffer is refused frame after frame by rmtWs2812Transmit while the card would
        // otherwise report "driving N of N": the same silent freeze this change set out to remove.
        // Far less reachable at 3 bytes per light than at the old 96, but the fallback still exists.
        frameUnusable_ = !frame_ || platform::ptrIsPsram(frame_);
        if (frameUnusable_) {
            if (failBufEnsure()) {
                std::snprintf(failBuf_, kFailBufLen, frame_
                                  ? "%u lights need %u KB of internal RAM"
                                  : "out of memory for %u lights (%u KB)",
                              static_cast<unsigned>(n),
                              static_cast<unsigned>((need + 1023) / 1024));
                setStatus(failBuf_, Severity::Error);
            } else {
                setStatus(kNoFrameMemMsg, Severity::Error);
            }
            if (frame_) freeFrame();   // hand back memory the transmit will never read
        } else if (status() == failBuf_ || status() == kNoFrameMemMsg) {
            clearStatus();   // the count came back down (or the heap freed up): retract our error
        }
        publishHeapBytes();   // the frame buffer grew: refresh the memory readout
    }

    void freeFrame() {
        if (frame_) { platform::free(frame_); frame_ = nullptr; frameCap_ = 0; publishHeapBytes(); }
    }

protected:
    // Matches DriverBase's visibility — a private override would silently hide the hook from any
    // future caller holding a DriverBase*. ParallelLedDriver keeps it protected for the same reason.
    /// This driver's heap = the base scratch + the RMT symbol buffer (one word per WS2812 data bit,
    /// the driver's largest buffer). Summed for the per-module memory readout — see
    /// DriverBase::driverHeapBytes.
    size_t driverHeapBytes() const override {
        return DriverBase::driverHeapBytes() + frameCap_;
    }

private:

    // --- loopback self-test (control-driven) ---

    // Run the one-shot RMT TX→RX loopback on the FIRST pin and report via the
    // MoonModule status slot. The slot stores a const char* (no copy), so PASS /
    // jumper-missing / not-supported are flash literals — zero RAM. Only the
    // FAIL case needs the captured hex, so it borrows a buffer allocated ON
    // DEMAND and freed by clearFailBuf() (release + every non-FAIL outcome) —
    // no permanent member.
    void runLoopbackSelfTest() {
        if constexpr (platform::rmtTxChannels == 0) {
            clearFailBuf();
            setStatus("loopback: not supported on this platform", Severity::Warning);
            return;
        }
        if (pinCount_ == 0) {
            clearFailBuf();
            setStatus("loopback: no valid pins", Severity::Warning);
            return;
        }
        // The RX pin must be set: the loopback captures TX→RX over a jumper, so an
        // unset rxPin (-1) has nothing to listen on. Guard before the uint8_t cast
        // below, which would otherwise turn -1 into GPIO 255 (a bogus pin).
        if (loopbackRxPin < 0) {
            clearFailBuf();
            setStatus("loopback: set loopbackRxPin (jumper it to the TX pin)", Severity::Status);
            return;
        }
        // The test reconfigures the first data pin as TX, so release ALL our TX
        // channels first — this also guarantees the test's RX channel can always
        // allocate RMT memory, even with every TX channel otherwise claimed;
        // reinit() restores them after.
        deinitAll();
        // TX override: when loopbackTxPin is set, transmit on it instead of the
        // first data pin, so the bench loopback runs on a dedicated jumper without
        // re-typing `pins`. Falls back to pins[0] when unset.
        const uint8_t txPin = loopbackTxPin >= 0
            ? static_cast<uint8_t>(loopbackTxPin)
            : static_cast<uint8_t>(pinList_[0]);
        const uint8_t rxPin = static_cast<uint8_t>(loopbackRxPin);
        platform::RmtLoopbackResult r;
        if (loopbackFrame) {
            // Whole-frame stress test on the first pin's slice (or 64 lights if
            // no buffer is wired yet) — the size that actually exposes
            // frame-rate / RF corruption.
            const uint16_t lights = pinCounts_[0] > 0
                ? static_cast<uint16_t>(pinCounts_[0]) : 64;
            const uint8_t ch = correction_.outChannels ? correction_.outChannels : 3;
            r = platform::rmtWs2812LoopbackFrame(txPin, rxPin, lights, ch);
        } else {
            r = platform::rmtWs2812Loopback(txPin, rxPin);
        }
        reinit();
        if (!r.jumperDetected) {
            clearFailBuf();
            setStatus("loopback: jumper not detected", Severity::Warning);
        } else if (r.pass) {
            // PASS is a static literal (no failBuf_ alloc): setStatus holds the
            // pointer, not a copy, so the string must outlive the call, and
            // failBuf_ is by invariant a FAIL-only buffer (see clearFailBuf).
            // The whole-frame bit count is in the serial log; the status slot
            // doesn't need it badly enough to break either rule.
            clearFailBuf();
            setStatus("loopback PASS", Severity::Status);
        } else {
            failBufEnsure();
            if (failBuf_ && loopbackFrame) {
                // bits per light = outChannels × 8 (24 for RGB, 32 for RGBW) —
                // the same channel count the frame was built with, not a
                // hardcoded /24, so the light index is right for RGBW too.
                const unsigned bitsPerLight =
                    (correction_.outChannels ? correction_.outChannels : 3u) * 8u;
                std::snprintf(failBuf_, kFailBufLen,
                              "loopback FAIL: bad bit %u/%u (light %u)",
                              static_cast<unsigned>(r.firstBadBit),
                              static_cast<unsigned>(r.bitsChecked),
                              static_cast<unsigned>(r.firstBadBit / bitsPerLight));
                setStatus(failBuf_, Severity::Error);
            } else if (failBuf_) {
                std::snprintf(failBuf_, kFailBufLen, "loopback FAIL: sent %02X%02X%02X got %02X%02X%02X",
                              r.sent[0], r.sent[1], r.sent[2], r.got[0], r.got[1], r.got[2]);
                setStatus(failBuf_, Severity::Error);
            } else {
                setStatus("loopback FAIL", Severity::Error);
            }
        }
    }

    // --- RMT channels (hardware; RMT targets only) ---

    static constexpr const char* kInitFailMsg = "RMT init failed, check the pins";
    static constexpr const char* kNoFrameMemMsg = "out of memory for this many lights";

    // All-or-nothing: a failing pin deinits everything and reports which pin,
    // so tick()'s guard stays a single bool and the user sees one clear error
    // instead of some strands dark, some lit.
    void reinit() {
        if constexpr (platform::rmtTxChannels == 0) return;
        deinitAll();
        if (pinCount_ == 0) return;   // parse error — already in the status slot
        for (uint8_t i = 0; i < pinCount_; i++) {
            if (platform::rmtWs2812Init(rmt_[i], static_cast<uint8_t>(pinList_[i]),
                                        kResolutionHz, cfg_.invert)) {
                pushBitTiming(i);   // the expander needs the bit shapes before the first frame
                continue;
            }
            // Surface which pin failed instead of silently no-op'ing in tick() —
            // the status tells the user why output is dark (usually a bad pin),
            // rather than leaving them to wonder why nothing lights.
            deinitAll();
            clearFailBuf();
            if (failBufEnsure()) {
                std::snprintf(failBuf_, kFailBufLen, "RMT init failed on pin %u",
                              static_cast<unsigned>(pinList_[i]));
                setStatus(failBuf_, Severity::Error);
            } else {
                setStatus(kInitFailMsg, Severity::Error);
            }
            return;
        }
        inited_ = true;
        // A prior init failure recovered (e.g. a pin fixed): drop the stale error. NOT when
        // resizeFrame left the symbol-buffer error there: reinit() runs right after it on every
        // rebuild, and failBuf_ carries both messages, so an unguarded clear here retracted the
        // "needs N KB internal RAM" error microseconds after it was set and the card fell back to
        // a blank status while the strip stayed frozen (bench, 900 lights on a Dig-Next-2).
        if (failBuf_ && status() == failBuf_ && !frameUnusable_) clearFailBuf();
        if (status() == kInitFailMsg) clearStatus();
    }

    // Releases only the RMT channels — NOT the symbol buffer (that's
    // freeFrame(), owned by release). reinit() calls this on every rebuild,
    // so freeing the buffer here would strand tick() — the original bug.
    void deinitAll() {
        if constexpr (platform::rmtTxChannels == 0) return;
        for (uint8_t i = 0; i < kMaxPins; i++) {
            if (rmt_[i].impl) platform::rmtWs2812Deinit(rmt_[i]);
        }
        inited_ = false;
    }
};

} // namespace mm
