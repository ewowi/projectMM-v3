#pragma once

#include "core/MoonModule.h"
#include "core/ActiveInstance.h"   // the one-active-mic election (the seat + its RAII vacate)
#include "core/AudioFrame.h"
#include "core/AudioLevel.h"
#include "core/AudioBands.h"
#include "core/math8.h"      // beatsin8 / sin8, the simulated-audio oscillators
#include "light/WLEDAudioSyncPacket.h"   // WLED audio-sync wire format (send/receive)
#include "platform/platform.h"

#include <cstdint>
#include <cstdio>   // snprintf for the read-out strings
#include <cstring>

namespace mm {

/// Acquires an audio source and publishes an `AudioFrame`: an overall sound
/// **level**, a 16-band frequency **spectrum**, and the **dominant peak**: as the
/// producer half of the audio-reactive pipeline
///
/// The frame is available to consumers every render tick, but its analysed values
/// are *recomputed* only when a full sample block has accumulated (a 512-sample
/// block at 22 kHz takes ~23 ms, longer than one tick), so a tick that doesn't
/// complete a block re-publishes the previous `AudioFrame` unchanged rather than
/// re-analyzing. `AudioSpectrumEffect` and the other audio effects are the consumers,
/// reaching the live frame through the static `latestFrame()`.
///
/// **Named for what it does**: audio acquisition plus analysis, not for one
/// source. Today the source is a digital I2S MEMS microphone (INMP441-class, the
/// only one wired); the same source-independent analysis pipeline is built to serve
/// other sources (line-in, USB audio, PDM mics, I2C codecs) behind the platform
/// read seam as they are added. Most of the module is the analysis (DC-blocker,
/// RMS level, windowed FFT, band mapping), which is source-independent.
///
/// **User-added Service.** A child of the `Services` container, registered in the
/// factory and added through the UI when wanted, not boot-wired, auto-wiring it
/// forced an I2S init on every board, which on the classic ESP32 hung `setup()` and
/// boot-looped a mic-less device. When added, its pins default to unset (−1, the
/// standard Pin-control sentinel, so GPIO 0 stays a usable mic pin) and it stays
/// idle with a status note until the user enters the real GPIOs. Chip-agnostic:
/// gated on `platform::hasAudioInput`: a pin-wired I2S mic on boards, an OS capture
/// device on desktop (the `device` Select: the system default mic out of the box, and
/// loopback devices such as BlackHole when installed, so effects can follow what the
/// machine plays). A desktop in Local mode with "send audio" on is a WLED audio-sync
/// SOURCE: one machine's capture drives a whole fleet of boards in Receive mode.
///
/// **The AudioFrame pipeline.** Each `tick()` that completes a block: read a block
/// of samples, DC-blocker high-pass, compute the level, window + FFT, map to bands.
/// The high-pass conditions the raw block once, up front, so both the level and the
/// spectrum see the same cleaned signal. The DSP choices are textbook defaults on
/// purpose, a Hann window, RMS for level, a geometric band split, argmax for the
/// peak, with deliberately no per-frequency correction table (the INMP441 is flat
/// ±3 dB across the range that matters). The level is overall RMS loudness computed
/// independently of the FFT, not derived from the bands.
///
/// **Hardware: INMP441-class digital mic.** A self-clocked I2S MEMS microphone:
/// standard/Philips framing, 24-bit data left-justified in a 32-bit slot, mono. The
/// part is self-clocked from the bit clock; there is no master-clock (MCLK) pin.
/// The bench wiring is SCK=6 (bit clock), WS=4 (word-select/LRCLK), SD=5 (serial data
/// out). It drives the one slot its L/R select pin chooses (tie L/R to GND for the
/// left slot); if `level` stays at the floor with sound present, the mic is filling
/// the other slot, one wire, not firmware.
///
/// **Platform seams.** Only the audio read and the FFT kernel are platform code
/// (boards: `platform_esp32_i2s.cpp`, IDF's `i2s_std` driver + esp-dsp's radix-2 FFT;
/// desktop: `platform_desktop_audio.cpp`, miniaudio capture into a lock-free ring +
/// a radix-2 Cooley-Tukey in `platform_desktop.cpp`); everything else is plain domain
/// math, host-tested in CI. The signal math is host-tested
/// domain code (`AudioLevel.h`, `AudioBands.h`); this module owns the lifecycle,
/// the controls, and the two seams.
///
/// **Hot path:** fixed member scratch buffers (sample block + window + magnitudes,
/// ~6 KB DRAM-resident), one float FFT per loop, no per-loop heap. The mic read is
/// non-blocking (the first ~250 ms of power-on settling garbage flows through the
/// first few reads and self-corrects); a bad init leaves the module idle (zeroed
/// frame), never crashing.
///
/// **Prior art:** audio-reactive lighting is a long-standing idea in the LED-controller world
/// (WLED-MM and MoonLight are the closest lineage). This is projectMM's own implementation, designed
/// from the INMP441 datasheet (https://invensense.tdk.com/wp-content/uploads/2015/02/INMP441.pdf) and
/// standard DSP rather than traced from any one project, studying, with credit, the thinking of
/// Frank (softhack007, WLED-MM audioreactive), Troy (troyhacks, the esp-dsp FFT + biquad pre-filters
/// path we share), and Damian Schneider (DedeHai, the fixed-point FFT for FPU-less chips). The
/// forward-looking analysis (source-seam extensions, line-in / PDM / analog / I²C codecs, and the
/// adaptive-noise-gate design that would retire the borrowed `floor` squelch) is a design study in
/// docs/backlog/audio-dsp-roadmap.md.
/// @card AudioService.png
class AudioService : public MoonModule {
public:
    /// Block size = FFT size: a power of two. 512 samples at 22050 Hz is ~23 ms of
    /// audio per frame, fine resolution (~43 Hz/bin) at a modest per-tick cost.
    /// What every producer of raw bands does next, once: the meter ballistic, the spectral flux
    /// and the onset decision. Four paths write `frame_.bands` (the mic, two simulations and a
    /// received sync packet); routing them all through here is what keeps the four consumers of
    /// the frame seeing one definition of "smoothed" and one definition of "a hit".
    void finishBands() {
        smoothBands(frame_.bands, frame_.bandsSmoothed);
        frame_.flux = spectralFlux(prevBands_, frame_.bands);
        std::memcpy(prevBands_, frame_.bands, sizeof(prevBands_));
        frame_.onset = onset_.feed(frame_.flux, platform::millis()) ? frame_.flux : 0;
        if (frame_.onset) onsetCount_++;
        if (frame_.flux > fluxPeak_) fluxPeak_ = frame_.flux;
    }
    uint8_t       prevBands_[16] = {};   ///< last block's raw bands, the flux's reference
    BandConditioner cond_;               ///< the per-band floor and peak tables, learned live
    LevelConditioner levelCond_;         ///< the same, for the overall level (VU) in automatic mode
    OnsetDetector onset_;                ///< the hit decision, with its running mean and refractory

    static constexpr size_t kBlock = 512;
    static constexpr size_t kMag = kBlock / 2;   ///< real-FFT magnitude bins

    ModuleRole role() const MM_NONBLOCKING override { return ModuleRole::Service; }


    /// Unlike a zero-cost diagnostic peripheral, this module pays a
    /// real per-tick cost (the FFT) that IS the capability, not an optional extra,
    /// so it must not run when the user turns it off. We therefore respect `enabled`
    /// (the default): the Scheduler skips tick() entirely while disabled, so the FFT,
    /// the level, and the read-outs all stop and the cost goes to zero. Enabled runs
    /// the full pipeline; removing the module stops it the same way. The read-outs
    /// hold their last value while disabled (no consumer reads a disabled module).
    /// (respectsEnabled() defaults to true, so we don't override it.)

    // --- controls: three I2S pins, sample rate, the two conditioning knobs, and
    // two read-only read-outs. The pins default to UNSET (-1, the standard Pin-
    // control sentinel, so GPIO 0 stays a usable mic pin): the module is user-added
    // when a board has a mic, and stays idle (no I2S init) until the user enters the
    // real GPIOs, so adding it can't grab arbitrary pins or wedge a board with no
    // mic. The bench INMP441 wiring is SCK=6 / WS=4 / SD=5. Order follows the I2S
    // datasheet convention: clocks (SCK, WS) then data (SD) then the optional MCLK. ---
    /// OS capture device (desktop): an index into platform::audioCaptureDevices' list.
    /// 0 = "default" (order-stable). Changing it re-opens capture live (no reboot).
    uint8_t device = 0;
    /// Which kind of microphone is wired: 0 = I2S (three wires, an INMP441-class PCM part),
    /// 1 = PDM (two wires, the one-bit part boards solder on, such as the QuinLED Dig-Next-2's).
    /// PDM has no bit clock and no master clock, so those two pins hide when it is selected.
    uint8_t micMode = 0;
    int8_t sckPin = -1;          ///< bit clock / BCLK (-1 = unset). Changing it re-creates the I2S channel live (no reboot).
    int8_t wsPin = -1;           ///< word-select / LRCLK (-1 = unset). Changing it re-creates the I2S channel live.
    int8_t sdPin = -1;           ///< serial data in / DOUT (-1 = unset). Changing it re-creates the I2S channel live.
    int8_t mclkPin = -1;         ///< master clock out (-1 = none). Self-clocked MEMS mics (INMP441) leave this
                                 ///< -1; an ADC/codec that needs a master clock (e.g. the MHC-WLED P4 shield's
                                 ///< line-in ADC on GPIO3) sets it so the I2S peripheral drives MCLK. On a codec
                                 ///< board (CodecType != None) the codec's own mclk is used instead (see reinit()).
                                 ///< Changing it re-creates the I2S channel live.
    /// Sample rate is a discrete choice (the standard audio rates), so it's a
    /// dropdown over a fixed set, not a free number. sampleRateSel indexes
    /// kSampleRates; sampleRate() resolves it to Hz. Default index 2 = 22050
    /// (~11 kHz Nyquist covers the range that matters for light). Changing it
    /// re-creates the channel live.
    uint8_t  sampleRateSel = 2;
    // Two knobs condition the spectrum + level:
    uint8_t  floor = 100;        ///< noise floor (dB display floor), bands/level
                                 ///< below this read as silence. Raise to keep an
                                 ///< ambient room dark, lower for a quiet room.
    uint8_t  gain = 128;         ///< sensitivity, HIGHER = more (a narrower dB window
                                 ///< so a given sound fills more of the bar).
    /// Per-band conditioning (AudioBands.h, BandConditioner): the learner that levels the RIG,
    /// mic response and room, without touching the music's own balance. `agc` picks whether the
    /// tables keep learning; `ratio` is a compressor's N:1 (1 = off); `maxGain` caps the lift in
    /// dB so a silent band is never amplified into its own noise.
    /// Who sets the display window: 0 = manual (the floor and gain sliders), 1 = automatic (the
    /// per-band learner). One choice rather than two overlapping mechanisms, so a slider on screen
    /// is always a slider that does something.
    uint8_t  levels = 1;
    /// Automatic levelling, fixed rather than exposed. Both act on the LEARNED per-band range,
    /// which the conditioner has already normalized per rig, so one value serves every source: a
    /// PDM mic, an INMP441 and a line-in all arrive looking the same. What differs between them is
    /// the absolute level, and that is `floor`'s job, the one knob automatic mode keeps.
    ///
    /// 4 = N of N:1, correcting three quarters of a band's deviation: enough to take out the rig's
    /// coloration, short of the high ratios that cause cross-spectral pumping. 24 dB caps the lift
    /// for a rig whose bands sit far apart; with silence gated it rarely binds, and it is kept as
    /// the guard rather than a tuning knob.
    static constexpr uint8_t kRatio = 4;
    static constexpr uint8_t kMaxGainDb = 24;
    /// Simulated-audio pattern (only shown, and only used, in Simulate mode, see `mode`). The synthesized
    /// signal drives audio-reactive effects with no mic or music, for a preview/demo device or a test:
    ///   `music`: a plausible song: multi-sine bands + a swelling volume + a periodic beat + a
    ///             sweeping peak. Nice for demos (bars dance, VU breathes, peaks move).
    ///   `sweep`: a single band lit, marching bass→treble on a timer, with the peak frequency and a
    ///             steady volume tracking it. Deterministic, the clean test pattern to check that each
    ///             effect responds across the whole spectrum.
    uint8_t  simulate = 0;       ///< 0 = music, 1 = sweep (the pattern; Simulate mode only)
    /// The module's audio SOURCE, the first thing to pick (below status). Three either/or modes, each with
    /// its own detail controls (the others hide): 0 = Local audio (its own peripheral, the on-board mic /
    /// line-in; analyze it here and optionally broadcast it, so pins, rate, floor/gain and "send audio"
    /// show). 1 = Receive network (a pure network sink: bind the sync port and let a peer's AudioFrame drive
    /// the effects, no local peripheral). 2 = Simulate (a synthesized source for demos/tests, only the
    /// `simulate` pattern picker shows). A device is exactly one of these at a time. Changing it re-runs
    /// prepare() (acquires/releases the mic, rebinds/unbinds the socket) and re-toggles which controls
    /// show, all live.
    uint8_t  mode = 0;           ///< 0 = local audio, 1 = receive network, 2 = simulate
    /// The `mode` value that means Simulate. "receive network" (index 1) only exists where the network
    /// does, so Simulate is index 2 with a network stack and index 1 without, the whole mode set shifts,
    /// and every mode comparison uses this constant rather than a bare literal (defineControls + tick).
    static constexpr uint8_t kSimMode = platform::hasNetwork ? 2 : 1;
    /// Broadcast this device's AudioFrame over UDP (WLED v2 wire format) for WLED / MoonLight
    /// receivers. Only meaningful, and only shown, in Local mode (there's nothing to send when
    /// you're a network sink). Off by default: a fresh module analyzes locally and broadcasts
    /// nothing until you opt in.
    bool     send = false;
    /// The three source/sink states the sync machinery keys off, derived from mode + send so the
    /// socket/tick logic stays a single 0/1/2 switch (0 = no socket, 1 = broadcast, 2 = network sink):
    /// Local+send → send, Local alone → off (local-only, no socket), Receive → receive, Simulate → off.
    /// `send` counts ONLY in Local mode (mode 0): a persisted send=true must not broadcast in Simulate
    /// mode, which has no captured frame worth sending. The `hasNetwork` guard on the Receive leg matters
    /// on a no-network build: there mode 1 is Simulate (not Receive, see kSimMode), so without it a
    /// Simulate device would wrongly read as "network sink" (2).
    uint8_t  sync() const { return (platform::hasNetwork && mode == 1) ? 2 : (mode == 0 && send ? 1 : 0); }
    /// The sync UDP port, the Send destination and the Receive listen port. Defaults
    /// to WLED's 11988 (interop with WLED/MoonLight); set it the same on both ends to
    /// run a private projectMM-only sync group on a non-WLED port.
    uint16_t syncPort = WLED_SYNC_PORT;

    static constexpr uint16_t kSampleRates[] = {8000, 16000, 22050, 44100};
    static constexpr uint8_t kSampleRateCount = 4;
    uint32_t sampleRate() const { return kSampleRates[sampleRateSel < kSampleRateCount
                                                       ? sampleRateSel : 2]; }

    void defineControls() override {
        // `mode` is the module's identity, so it's the first control (below status). Everything else is
        // its detail: the local-audio group shows only in Local mode, the simulate pattern only in Simulate
        // mode, the sync group only when there's a socket. Registration order follows the dependency (the
        // coding-standards rule): the gate first, the controls it gates under it, mutually-exclusive groups
        // sharing a gate grouped after it. The "receive network" option only exists where the network does
        // (Local + Simulate always; Receive added when platform::hasNetwork), so Simulate is index 1 on a
        // no-network build and 2 with network, the class constant kSimMode carries that shift.
        const bool localMode = (mode == 0);
        const bool simMode = (mode == kSimMode);
        if constexpr (platform::hasNetwork) {
            static constexpr const char* kModeOptions[] = {"local audio", "receive network", "simulate"};
            controls_.addSelect("mode", mode, kModeOptions, 3);
        } else {
            static constexpr const char* kModeOptions[] = {"local audio", "simulate"};
            controls_.addSelect("mode", mode, kModeOptions, 2);
        }
        // --- Local-audio group: the audio input and its analysis. Shown only in Local mode. On
        // I2S targets that means the mic pins (default UNSET, -1, so adding the module can't grab
        // GPIOs; order follows the I2S datasheet: clocks, data, optional MCLK). On desktop it is
        // the OS capture device instead. ---
        if constexpr (platform::hasI2sMic) {
            // A PDM part has neither a bit clock nor a master clock: its two wires are the clock
            // the chip drives (wsPin) and the data line (sdPin). Showing the other two would
            // invite a user to set pins nothing reads.
            static constexpr const char* kMicModeOptions[] = {"I2S", "PDM"};
            controls_.addSelect("micMode", micMode, kMicModeOptions, 2);
            controls_.setHidden(controls_.count() - 1, !localMode);
            const bool pdm = micMode == 1;
            controls_.addPin("sckPin", sckPin);        controls_.setHidden(controls_.count() - 1, !localMode || pdm);
            controls_.addPin("wsPin", wsPin);          controls_.setHidden(controls_.count() - 1, !localMode);
            controls_.addPin("sdPin", sdPin);          controls_.setHidden(controls_.count() - 1, !localMode);
            controls_.addPin("mclkPin", mclkPin);      controls_.setHidden(controls_.count() - 1, !localMode || pdm);
        }
        if constexpr (platform::hasAudioCapture) {
            // The OS capture input: entry 0 "default" follows the system setting; loopback
            // devices (BlackHole and friends) appear when installed. Re-enumerated on every
            // rebuild, so a hot-plugged device shows up on the next control change.
            //
            // Persisted by LABEL (setPersistLabel, as the NIC and HLS Selects do), because this
            // list is LIVE ENUMERATION and its indices are not stable: unplug a webcam or let a
            // Continuity Camera drop off and every device below it shifts up a slot, so a saved
            // index silently starts naming a different device. That matters most for exactly the
            // case someone sets up deliberately, a loopback like BlackHole, which is routing meant
            // to stay put rather than a mic one would re-pick. Matching by name survives it; a
            // device that is genuinely gone falls back to "default" at 0.
            const char* const* deviceOptions = nullptr;
            const uint8_t deviceCount = static_cast<uint8_t>(platform::audioCaptureDevices(&deviceOptions));
            controls_.addSelect("device", device, deviceOptions, deviceCount);
            controls_.setPersistLabel(controls_.count() - 1);
            controls_.setHidden(controls_.count() - 1, !localMode);
        }
        static constexpr const char* kRateOptions[] = {"8000", "16000", "22050", "44100"};
        controls_.addSelect("sampleRate", sampleRateSel, kRateOptions, kSampleRateCount);
        controls_.setHidden(controls_.count() - 1, !localMode);
        // floor/gain condition the local FFT/level mapping.
        // ONE decision, then the controls that decision needs. `levels` says who sets the display
        // window: a person (the floor and gain sliders) or the learner (which measures each band's
        // own floor and typical peak and maps them onto the window for you). Showing both sets at
        // once was the confusing part: four sliders for two jobs, with `agc` silently overriding
        // what `floor` and `gain` meant while leaving them on screen.
        static constexpr const char* kLevelsOptions[] = {"manual", "automatic"};
        controls_.addSelect("levels", levels, kLevelsOptions, 2);
        controls_.setHidden(controls_.count() - 1, !localMode);
        const bool manual = levels == 0;
        // `floor` is shown in BOTH modes because it means one thing in both: below this is not
        // signal. Manual maps the display from it; automatic gates silence with it, which is what
        // stops the learner amplifying an empty room to full scale. `gain` sets the manual window's
        // span and has no automatic counterpart, so it hides with the mode that uses it.
        controls_.addControl("floor", floor, 0, 255); controls_.setHidden(controls_.count() - 1, !localMode);
        controls_.addControl("gain", gain, 1, 255);   controls_.setHidden(controls_.count() - 1, !localMode || !manual);
        // Automatic exposes no levelling knobs: `strength` and `maxBoost` measurably changed the
        // numbers but nothing a viewer could see once silence was gated, and both are
        // source-independent (see kRatio), so they are constants.
        // "send audio": broadcast the locally-analyzed frame. Only meaningful in Local mode.
        if constexpr (platform::hasNetwork) {
            controls_.addControl("send audio", send);
            controls_.setHidden(controls_.count() - 1, !localMode);
        }
        // --- Simulate group: the synthesized-pattern picker, shown only in Simulate mode. ---
        static constexpr const char* kSimulateOptions[] = {"music", "sweep"};
        controls_.addSelect("simulate", simulate, kSimulateOptions, 2);
        controls_.setHidden(controls_.count() - 1, !simMode);
        // --- Sync group: only relevant when a socket is bound, i.e. sending (Local + send) or receiving.
        // Both the port and the live status hide when there's no socket. A mode/send change re-runs this
        // method (both are in affectsPrepare) so the rows toggle live. ---
        if constexpr (platform::hasNetwork) {
            const bool hasSocket = (sync() != 0);
            // The UDP port, the Send destination and the Receive listen port. Defaults to WLED's
            // 11988 (interop with WLED/MoonLight); change it on BOTH ends to run a private
            // projectMM-only sync group on a non-WLED port.
            controls_.addControl("syncPort", syncPort, 1, 65535);
            controls_.setHidden(controls_.count() - 1, !hasSocket);
        }
        // Read-only live read-outs (formatted in tick1s). These show the audio actually driving the
        // effects, so they stay visible in Receive too (there the frame comes off the network, not a
        // mic). Derived every second, nothing to persist, so ReadOnly not a flipped Text.
        // "level RMS" = the RMS loudness; the DISPLAYED number is its peak over the 1-second window
        // (tick1s publishes levelPeak_, the max of the per-block RMS level, then resets it), so a
        // beat that lands between samples still registers. The live frame_.level the LEDs use is the
        // instantaneous RMS, recomputed every audio block, this read-out is the human-readable
        // summary of it, not a separate statistic.
        controls_.addReadOnly("level RMS", levelStr_, sizeof(levelStr_));
        // The onset diagnostic: hits per second and the peak flux, the one row that answers
        // "is the detector hearing the beat" without a per-band list (audio roadmap, § The UI).
        controls_.addReadOnly("onsets", onsetStr_, sizeof(onsetStr_));
        controls_.addReadOnly("peakHz", peakStr_, sizeof(peakStr_));
        MoonModule::defineControls();
    }

    /// A pin or rate change rebuilds the I2S channel (live, no reboot); a `mode` / `send` /
    /// `syncPort` change re-binds/unbinds the UDP socket AND re-toggles which control rows show
    /// (all flow through prepare → rebuildControls).
    bool affectsPrepare(const char* name) const override {
        return std::strcmp(name, "wsPin") == 0 || std::strcmp(name, "sdPin") == 0
            || std::strcmp(name, "sckPin") == 0 || std::strcmp(name, "mclkPin") == 0
            // Re-creates the I2S channel in the other mode, AND hides or shows the two clock
            // pins PDM does not have.
            || std::strcmp(name, "micMode") == 0
            || std::strcmp(name, "device") == 0
            || std::strcmp(name, "sampleRate") == 0 || std::strcmp(name, "mode") == 0
            || std::strcmp(name, "send audio") == 0 || std::strcmp(name, "syncPort") == 0
            // `levels` swaps which sliders are shown (manual floor/gain against the learner's
            // strength/maxBoost), so it toggles rows exactly as mode and send do.
            || std::strcmp(name, "levels") == 0;
    }

    /// Pure build (see MoonModule::prepare): claim the frame election (this instance's frame_ drives the
    /// effects in EVERY mode, a live mic, a received peer frame, or a synthesized one), then acquire only
    /// the hardware the current `mode` needs. Only Local runs a real peripheral, so only Local inits the
    /// I²S mic; Receive (a network sink) and Simulate (a synthetic source) free the I²S channel and its
    /// pins. The sync socket is rebound in every mode (syncReinit is a no-op unless there's a socket to
    /// bind). No enabled() check, core's applyState() calls this only when effectively-enabled and routes
    /// to release() otherwise.
    void prepare() override {
        micSeat_.claim();       // first live instance wins the frame seat (claim-if-empty), any mode
        if (mode == 0) {
            reinit();           // Local only: (re)acquire the I²S mic (sets its own mic status)
        } else {
            deinit();           // Receive / Simulate: no peripheral, free the I²S channel + its pins
            clearStatus();      // the mic status is a Local-mode diagnostic; clear it so a stale
                                // "mic: set sckPin…" doesn't linger on the status row (Receive/Simulate
                                // report through the separate sync-status row instead)
        }
        syncReinit();
    }
    /// One-time wiring only; the mic acquire + election live in prepare(), the sole gate.
    void setup() override {}
    void release() override {
        deinit();
        if constexpr (platform::hasNetwork) { syncSock_.close(); syncOpen_ = false; }
        micSeat_.vacate();       // vacate the seat; a surviving mic re-claims it in tick()
        MoonModule::release();   // chain: free any registered buffers + recurse (override-and-chain convention)
    }

    /// The latest analyzed frame, what effects read. Always valid (zeroed until
    /// the first successful read), so a consumer never dereferences null and a
    /// mic-less build just sees silence.
    const AudioFrame* audioFrame() const { return &frame_; }

    // --- Test seams (host unit tests only; mirror DevicesModule::injectPacketForTest) ---
    // Read-only views of the sync socket lifecycle so unit_AudioService_sync can assert it
    // through the public tick() without befriending the class or exposing internals broadly.
    bool syncOpenForTest() const { return syncOpen_; }
    /// The sync state as the card shows it. Reads the module's own status line rather than a
    /// private buffer, because that IS the reported state now: a test asserting on anything else
    /// could pass while the card said something different.
    const char* syncStatusForTest() const { return status() ? status() : ""; }
    static constexpr uint32_t syncSendIntervalMsForTest() { return kSyncSendIntervalMs; }
    uint32_t syncSendCountForTest() const { return syncSendCount_; }
    static constexpr uint32_t syncFallbackMsForTest() { return kSyncFallbackMs; }
    static constexpr uint32_t syncOpenRetryMsForTest() { return kSyncOpenRetryMs; }

    /// Process-wide accessor for the consumers (audio effects). There is one mic,
    /// and an effect can be added/removed via the UI at any time, so it can't rely
    /// on a boot-time setter, it asks here
    ///
    /// Returns the live mic's frame while one exists, else a static all-silent
    /// frame, so an effect added before/without a mic still reads valid silence
    /// instead of null. The FIRST live module claims the seat in `prepare()`, vacates
    /// it in `release()`, and any running module re-claims an empty seat in
    /// `tick()`: so a device with two mics reads the first consistently, and
    /// removing the active one lets a survivor take over. Add/remove in any order
    /// leaves a coherent answer (the robustness rule).
    static const AudioFrame* latestFrame() MM_NONBLOCKING {
        // constexpr, not a function-local static: a static needs a guard variable and a
        // one-time lock on first use, which is a blocking operation on the render path, and
        // this is called from EIGHT audio-reactive effects' tick(). constexpr is initialized at
        // compile time, so there is no guard and no lock.
        static constexpr AudioFrame kSilence{};
        AudioService* a = ActiveInstance<AudioService>::active();
        return a ? &a->frame_ : &kSilence;
    }

    void tick() MM_NONBLOCKING override {
        // Self-elect as the active mic if the seat is empty. prepare() gives it to the first live
        // module and release() vacates it, but removing the active module while a second one is still running
        // would otherwise leave the seat empty (effects go silent). A running module re-claiming an
        // empty seat here keeps latestFrame() pointing at a live frame for ANY add/remove order, the
        // survivor takes over on its next tick (robustness). claim() is idempotent, a no-op while the
        // seat is held, the reclaim once it's empty.
        micSeat_.claim();

        // WLED audio sync. Send (Local + send audio) broadcasts the current frame_ (throttled),
        // then falls through to run the local mic analysis that produces that frame_. Receive is a
        // pure network sink: drain the socket into frame_ and RETURN unconditionally so the local
        // mic path never runs (there is no peripheral in this mode), holding the last frame while a
        // peer is quiet rather than blending to a mic.
        if constexpr (platform::hasNetwork) {
            const uint8_t s = sync();
            if (s != 0 && syncEnsureSocket()) {   // lazy-open once the network is up
                if (s == 1) syncSend();
                else if (s == 2) { syncReceive(); return; }
            }
            if (s == 2) return;   // sink with no socket yet: still never runs the local mic
        }

        // Simulate mode (see `mode`): a synthesized source, driven by the `simulate` pattern (0 = music,
        // 1 = sweep). It's a whole mode, not a mic fill-in, so it always runs and returns, the mic path
        // below never executes in Simulate mode.
        if (mode == kSimMode) { synthesizeFrame(simulate == 1); return; }

        // From here on it's Local mode. With no audio input on the platform, or before a good
        // init, nothing is produced, the last frame is held (no synthetic fallback; that's
        // Simulate mode's job). hasAudioInput covers both worlds: a pin-wired I2S mic on
        // boards, an OS capture device on desktop.
        if constexpr (!platform::hasAudioInput) {
            return;
        } else {
            if (!inited_) return;
        }

        // Drain whatever the DMA holds this tick (non-blocking) into the free tail
        // of the block accumulator. A full kBlock takes ~23 ms to arrive (longer
        // than one render tick), so each tick contributes a partial; we analyse
        // once the accumulator is full, then reset it.
        const size_t n = platform::audioMicRead(mic_, samples_ + filled_, kBlock - filled_);
        // Mic-health tallies (read here, before the DC-blocker rewrites samples_): the samples the I2S
        // read delivered, and whether any were non-zero, accumulated over the 1 s window and diagnosed
        // in tick1s(). The two together isolate the two clean failure modes of a mic bring-up: samples
        // arriving proves the clocks (BCLK/WS) + DMA; a non-zero among them proves the data line (SD).
        // All-zero samples = clocks fine, SD dead, otherwise a silent "RMS 0" with no clue which wire.
        micSamples1s_ += n;
        for (size_t i = 0; i < n; i++) if (samples_[filled_ + i] != 0) { micNonzero1s_++; break; }
        if (n == 0) return;                            // nothing ready this tick
        filled_ += n;
        if (filled_ < kBlock) return;                  // wait for a whole block
        filled_ = 0;                                   // consumed below; refill next

        // DC-blocker high-pass (~40 Hz): removes the constant offset + sub-bass
        // rumble before any analysis, so they can't leak into the low bands. The
        // filter is continuous across blocks (state in dc_).
        dc_.process(samples_, kBlock);

        // Level: overall loudness (RMS), independent of the FFT, it fluctuates
        // with how loud the room is. Uses a gentler floor than the bands (half),
        // so the VU keeps moving with volume instead of being gated hard like the
        // per-band display.
        computeLevel(samples_, kBlock, static_cast<uint8_t>(floor / 2), gain, frame_,
                     levels == 1 ? &levelCond_ : nullptr,
                     static_cast<uint32_t>(kBlock * 1000u / sampleRate()));

        // Smoothed level: a one-pole exponential moving average of the raw `level`, so effects that
        // want a calm, breathing VU (rather than the raw value's snap-to-transient) read a value that
        // lags and rounds off sudden changes. 3/4 old + 1/4 new is the textbook light smoothing,
        // fast enough to follow the music, slow enough to hide per-block jitter. Integer-only, one
        // block behind, off the per-light path. (WLED's `volume`/`volumeSmth` to our raw `level`.)
        frame_.levelSmoothed = static_cast<uint16_t>((frame_.levelSmoothed * 3 + frame_.level) / 4);

        // Spectrum: window -> FFT -> 16 log bands, same floor/gain mapping.
        uint16_t peakHz = 0, peakMag = 0;
        applyWindow(samples_, kBlock, windowed_);
        platform::audioFft(windowed_, kBlock, mag_);
        magnitudesToBands(mag_, kMag, sampleRate(), floor, gain,
                          frame_.bands, peakHz, peakMag,
                          levels == 1 ? &cond_ : nullptr,
                          static_cast<uint32_t>(kBlock * 1000u / sampleRate()),
                          kRatio, static_cast<float>(kMaxGainDb), true);
        finishBands();

        // Peak frequency: the exact-Hz FFT bin, held when there's no real signal so
        // it doesn't wander in silence.
        if (peakMag > 8) { frame_.peakHz = peakHz; frame_.peakMag = peakMag; }

        // Latch a beat for the audio-sync packet: raw level notably above the smoothed average,
        // at most one per refractory window. Analysis runs faster than the send, so latching here
        // (rather than testing at send time) reports each beat exactly once, which is what WLED's
        // udpSamplePeak does.
        const uint32_t nowMs = platform::millis();
        if (frame_.level > frame_.levelSmoothed + kSyncPeakMargin &&
            nowMs - lastPeakMs_ >= kSyncPeakRefractoryMs) {
            syncPeakLatched_ = true;
            lastPeakMs_ = nowMs;
        }

        // Track the PEAK level across the 1 s display window. frame_.level is recomputed every
        // ~23 ms audio block, but the UI string is snapshotted only once a second, sampling the
        // instantaneous value lands in the gaps between beats and reads 0 even while the LEDs (driven
        // live every render tick) move with the music. The window peak is the representative reading.
        // Display-only, the live frame_.level the effects/LEDs use is untouched.
        if (frame_.level > levelPeak_) levelPeak_ = frame_.level;
    }

    /// Fill frame_ with a synthesized signal. sweep=false → a plausible "song" (each band its own
    /// oscillator, a swelling volume, a periodic beat, a drifting peak); sweep=true → one band lit
    /// marching bass→treble on a timer (the deterministic test pattern). All integer LUT math (sin8),
    /// once per tick, off the per-light path. Also runs the same levelSmoothed EMA the mic path does.
    void synthesizeFrame(bool sweep) {
        const uint32_t t = platform::millis();
        if (sweep) {
            // One band lit at a time, stepping bass→treble every ~250 ms and wrapping. The lit band
            // ramps up-and-down (triangle) so it's not a hard on/off, and the peak frequency + volume
            // track the swept band so frequency-mapped and volume effects follow it too.
            const uint8_t pos = static_cast<uint8_t>((t / 250u) % 16u);
            const uint8_t env = triwave8(static_cast<uint8_t>((t % 250u) * 255u / 250u));  // 0..255 within a step
            for (uint8_t b = 0; b < 16; b++) frame_.bands[b] = (b == pos) ? env : 0;
            finishBands();
            frame_.level = env;
            frame_.peakHz = static_cast<uint16_t>(80 + pos * 700);   // bass→~10.6 kHz across the 16 steps
            frame_.peakMag = env;
        } else {
            // Musical "song": each band an independent sine at its own rate/phase (bass slow, treble
            // fast), a slow overall volume swell, and a periodic beat pulse that briefly lifts the low
            // bands + volume so beat-reactive effects fire. A drifting peak sweeps the dominant tone.
            const uint8_t beat = (t % 600u < 90u) ? static_cast<uint8_t>(triwave8(static_cast<uint8_t>((t % 600u) * 255u / 90u))) : 0;
            uint16_t sum = 0;
            for (uint8_t b = 0; b < 16; b++) {
                // Per-band oscillator: rate rises with b (treble flickers faster), phase spread by b.
                const uint8_t rate = static_cast<uint8_t>(1 + b);                   // BPM-ish multiplier
                const uint8_t osc = sin8(static_cast<uint8_t>(t * rate / 8u + b * 24u));
                uint16_t v = static_cast<uint16_t>((osc * 3u) / 4u);               // 0..191 base
                if (b < 4) v = static_cast<uint16_t>(v + beat / 2u);               // beat lifts the bass
                frame_.bands[b] = static_cast<uint8_t>(v > 255 ? 255 : v);
                sum = static_cast<uint16_t>(sum + frame_.bands[b]);
            }
            const uint8_t swell = sin8(static_cast<uint8_t>(t / 24u));             // slow volume breath
            uint16_t lvl = static_cast<uint16_t>(swell / 2u + sum / 32u + beat / 2u);
            frame_.level = lvl > 255 ? 255 : lvl;
            finishBands();
            // Peak drifts across the spectrum so freq-mapped effects move.
            frame_.peakHz = static_cast<uint16_t>(80 + sin8(static_cast<uint8_t>(t / 40u)) * 40u);
            frame_.peakMag = frame_.level;
        }
        frame_.levelSmoothed = static_cast<uint16_t>((frame_.levelSmoothed * 3 + frame_.level) / 4);
        // Feed the same 1 s peak-hold the mic path uses, so the "level RMS" display tracks the
        // synthesized level instead of reading a stale 0 (the display is peak-over-window, not live).
        if (frame_.level > levelPeak_) levelPeak_ = static_cast<uint8_t>(frame_.level);
    }

    void tick1s() MM_NONBLOCKING override {
        // The mirror of the LED driver's retry: on the classic ESP32 a PDM microphone and the
        // parallel LED bus both need I2S0, so whichever asks second is refused, and the loser would
        // otherwise stay silent until the user edited a control. Retry only while Local mode is
        // wanted and the mic is not up, and only once the platform says the instance is free, so a
        // genuinely bad pin set costs nothing per second. reinit() is the cold path prepare() runs.
        if (mode == 0 && !inited_
            && platform::audioMicSharedBusFree(micMode == 1 ? platform::MicMode::Pdm
                                                            : platform::MicMode::I2sStd)) reinit();
        std::snprintf(levelStr_, sizeof(levelStr_), "%u", static_cast<unsigned>(levelPeak_));
        std::snprintf(onsetStr_, sizeof(onsetStr_), "%u/s, flux %u",
                      static_cast<unsigned>(onsetCount_), static_cast<unsigned>(fluxPeak_));
        onsetCount_ = 0; fluxPeak_ = 0;
        std::snprintf(peakStr_, sizeof(peakStr_), "%u Hz", static_cast<unsigned>(frame_.peakHz));
        levelPeak_ = 0;   // reset for the next window

        // Mic-health diagnosis from the 1 s tallies (see the read path). Only for a live *direct* mic
        // (inited, not a sync receiver, no codec), a codec/sync/unset-pin path has its own status. The
        // split turns a silent mic into a wire-specific verdict:
        //   no samples at all  → the I2S clocks aren't running, check sckPin / wsPin.
        //   samples, all zero  → clocks fine, data line dead, check sdPin (SD/DOUT) + power.
        //   samples, non-zero  → data is flowing; clear any prior diagnosis.
        // Only Local mode has a real mic to diagnose, Receive and Simulate produce frame_ without one.
        // And only a pin-wired I2S mic gets a WIRE verdict: a desktop capture device delivering
        // silence is a quiet room or an idle loopback, not a wiring fault to alarm about.
        const bool directMicLive = platform::hasI2sMic && inited_ && mode == 0
                                   && platform::audioCodecType == platform::CodecType::None;
        if (directMicLive) {
            if (micSamples1s_ == 0)
                setStatus(micMode == 1 ? "mic: no samples, check wsPin (PDM clock)"
                                       : "mic: no samples, check sckPin / wsPin (I2S clocks)",
                          Severity::Warning);
            else if (micNonzero1s_ == 0)
                setStatus("mic: data line silent, check sdPin (SD/DOUT) + mic power", Severity::Warning);
            else if (micStatusStale_)
                setStatus("", Severity::Status);   // data flowing again, clear a prior diagnosis
            micStatusStale_ = (micSamples1s_ == 0 || micNonzero1s_ == 0);
        } else if (mode != 0) {
            // No mic to diagnose on this path (Receive or Simulate), so no diagnosis may be
            // OUTSTANDING either. Without this the flag kept whatever Local mode last set: a mic
            // fault, then a switch to Receive, and the sync line below stayed suppressed forever,
            // so "listening" and "receiving from <ip>" could never appear again until a mic that is
            // no longer being read happened to recover.
            //
            // Gated on the MODE rather than on directMicLive, because that also goes false when
            // Local audio FAILED TO INITIALIZE (`inited_` is false). Clearing the flag there let
            // "sending" overwrite the capture-init error a second later, which is the one message
            // that says why there is no audio.
            micStatusStale_ = false;
        }
        micSamples1s_ = 0;
        micNonzero1s_ = 0;
        // Live sync state on the module's OWN status line: "sending" / "receiving from <ip>" (peer
        // audio fresh) / "listening" (bound, no peer). This used to be a separate "sync status"
        // read-only control, which put a second status field on a card that already has one; a
        // module reports through setStatus like every other module.
        //
        // Skipped while a mic fault is showing (Local + send runs both paths): a wiring warning
        // outranks the routine note that packets are going out.
        // While the socket isn't open yet, the baseline syncReinit/syncEnsureSocket set
        // ("waiting for network" / "...failed") stands; only once open is the moment-to-moment state
        // reported.
        if constexpr (platform::hasNetwork) {
            const uint8_t s = sync();
            if (s != 0 && syncOpen_ && !micStatusStale_) {
                if (s == 1) setStatus("sending");
                else if (lastSyncRecv_ != 0
                         && platform::millis() - lastSyncRecv_ < kSyncFallbackMs) {
                    // Named, because "receiving" alone cannot tell a rig taking the right source
                    // from one locked onto a neighbour.
                    std::snprintf(syncStr_, sizeof(syncStr_), "receiving from %u.%u.%u.%u",
                                  syncPeer_[0], syncPeer_[1], syncPeer_[2], syncPeer_[3]);
                    setStatus(syncStr_);
                } else {
                    setStatus("listening");
                }
            }
        }
        MoonModule::tick1s();
    }

private:
    // The one-active-mic election: the FIRST live module to build claims the seat, release() vacates
    // it, and a survivor re-claims an empty seat in tick(). latestFrame() reads the winner. The seat
    // is per-type static inside ActiveInstance; the destructor vacates it, so a bare destruction never
    // dangles it. (The election SEAT, distinct from the platform mic `mic_` handle below.)
    ActiveInstance<AudioService> micSeat_{*this};

    platform::AudioMicHandle mic_;
    bool inited_ = false;
    size_t filled_ = 0;         // samples accumulated toward the next full block
    DcBlocker dc_;              // ~40 Hz high-pass, continuous across blocks

    // Fixed hot-path scratch, sized once, never reallocated. ~6 KB total
    // (2 KB samples + 2 KB windowed + 1 KB magnitudes), DRAM-resident.
    int32_t samples_[kBlock] = {};
    float windowed_[kBlock] = {};
    float mag_[kMag] = {};

    AudioFrame frame_;

    char levelStr_[12] = {};
    char onsetStr_[20] = {};
    uint8_t onsetCount_ = 0;  ///< onsets in the current 1 s display window (UI only)
    uint8_t fluxPeak_ = 0;    ///< peak flux in that window (UI only)
    char peakStr_[12] = {};
    uint8_t levelPeak_ = 0;   // peak frame_.level across the current 1 s display window (UI only)

    // Mic-health tallies over the 1 s window (see the read path + tick1s diagnosis). micSamples1s_ =
    // raw samples the I2S read delivered (proves clocks/DMA); micNonzero1s_ = reads that carried a
    // non-zero sample (proves the SD data line). micStatusStale_ = a diagnosis is currently posted, so
    // tick1s clears it once the mic recovers rather than clearing every second.
    uint32_t micSamples1s_ = 0;
    uint32_t micNonzero1s_ = 0;
    bool     micStatusStale_ = false;

    // WLED audio sync (light/WLEDAudioSyncPacket.h). One socket, bound only in Send/Receive.
    platform::UdpSocket syncSock_;
    uint32_t lastSyncSend_ = 0;      // millis of the last send (send throttle)
    uint32_t syncSendCount_ = 0;         // sends made; test-visible so the throttle can be observed
                                         // (NOT a wire field: byte 17 is WLED's reserved2)
    bool     syncPeakLatched_ = false;   // a beat seen since the last transmit (WLED's udpSamplePeak)
    uint32_t lastPeakMs_ = 0;            // when that beat was, for the refractory window
    uint32_t lastSyncRecv_ = 0;      // millis of the last received packet (receive auto-blend)
    uint8_t  syncPeer_[4] = {};      // source address of that packet, for the status line
    bool     syncOpen_ = false;      // socket opened for the current mode (lazy-open latch)
    uint32_t lastSyncOpenFailMs_ = 0;  // millis of the last failed open (0 = none); bring-up backoff
    char     syncStr_[40] = {};      // scratch for the "receiving from <ip>" status line
    static constexpr uint32_t kSyncSendIntervalMs = 25;   // ~40/s, WLED-friendly, well under a flood
    static constexpr uint32_t kSyncFallbackMs = 1000;     // no packet this long → resume local mic
    static constexpr uint32_t kSyncOpenRetryMs = 1000;    // pause between socket bring-up retries after a failure
    static constexpr int kSyncMaxRecvPerTick = 8;         // bounded non-blocking drain (sync is low-rate)

    static constexpr const char* kInitFailMsg = "mic init failed, check pins / rate";

    /// (Re)create the I2S channel for the current pins + rate. On a codec board the
    /// I2S peripheral drives MCLK first, then the codec is configured over I2C. Any
    /// unset pin (-1) or missing I2S support leaves the module idle with a status
    /// note rather than attempting an init. Called from setup() and prepare().
    void reinit() {
        if constexpr (platform::hasAudioCapture) {
            // Desktop: the OS capture device the `device` Select picked. Failure is a live
            // status, not a crash, a denied OS microphone permission lands here too.
            deinit();
            inited_ = platform::audioCaptureInit(mic_, device, sampleRate());
            if (!inited_) {
                setStatus("capture init failed, pick another device", Severity::Error);
                return;
            }
            dc_.reset();
            clearStatus();
            return;
        }
        if constexpr (!platform::hasI2sMic) {
            setStatus("mic: no audio input on this platform", Severity::Warning);
            return;
        }
        deinit();
        // Any pin unset (-1, the default until the user wires a mic): stay idle,
        // don't attempt an I2S init: initializing I2S on unset pins is what hung a
        // mic-less board's boot. GPIO 0 IS a valid mic pin now (the sentinel is -1,
        // not 0), so the guard tests < 0, not == 0.
        // A PDM part has two wires, not three: the clock this chip drives (wsPin) and the data
        // line (sdPin). Requiring a bit clock there would leave a correctly wired board sitting
        // at "set sckPin" forever, with a pin it does not have.
        const bool pdm = micMode == 1;
        if (wsPin < 0 || sdPin < 0 || (!pdm && sckPin < 0)) {
            setStatus(pdm ? "mic: set wsPin (clock) / sdPin (data)"
                          : "mic: set sckPin / wsPin / sdPin", Severity::Status);
            return;
        }
        // Bring up the I2S channel FIRST. Where MCLK comes from depends on the board:
        //  - Codec board (an analog mic behind an I2S codec, e.g. the S31's ES8311): the
        //    codec's MCLK pin from the per-target config (platform::audioCodecPins.mclk).
        //    The codec won't even answer I2C until that clock runs, so I2S precedes the
        //    codec config below.
        //  - No codec: the runtime `mclkPin` control, −1 for a self-clocked MEMS mic
        //    (INMP441), or a real pin for an I2S ADC that needs a master clock (the
        //    MHC-WLED P4 shield's line-in ADC on GPIO3, WLED's SR_DMTYPE=4).
        const int16_t mclk = platform::audioCodecType == platform::CodecType::None
                           ? mclkPin : static_cast<int16_t>(platform::audioCodecPins.mclk);
        inited_ = platform::audioMicInit(mic_, static_cast<uint16_t>(wsPin),
                                         static_cast<uint16_t>(sdPin),
                                         static_cast<uint16_t>(sckPin), mclk, sampleRate(),
                                         static_cast<platform::MicMode>(micMode));
        if (!inited_) {
            setStatus(kInitFailMsg, Severity::Error);
            return;
        }
        // Now configure the codec over I2C (MCLK is running). A no-op returning true
        // on direct-mic boards, so the call is uniform. The codec then streams its ADC
        // onto the I2S bus the read above drains.
        if (!platform::audioCodecInit(platform::audioCodecType, platform::audioCodecPins,
                                      sampleRate())) {
            deinit();   // tear the I2S channel back down, we couldn't bring the codec up
            setStatus("mic: codec init failed, check I2C wiring", Severity::Error);
            return;
        }
        dc_.reset();   // start the high-pass clean for the new stream
        // The INMP441 emits ~250 ms of power-on settling garbage after the clock
        // starts. The read is non-blocking (hot-path rule), so we can't drain a
        // fixed sample count here at init, the DMA has barely filled. Instead the
        // settling samples flow through the first few tick() reads and the level /
        // bands self-correct within that quarter-second; no separate discard is
        // needed, and the frame stays valid (zeroed) until then.
        // Clear any prior status now the mic is live, not just kInitFailMsg, but
        // also the "set wsPin / sdPin / sckPin" note from the unset-pin path, which
        // would otherwise persist and mislead after the user fills the pins in.
        clearStatus();
    }

    void deinit() {
        if constexpr (!platform::hasAudioInput) return;
        if (inited_) platform::audioMicDeinit(mic_);
        platform::audioCodecDeinit();   // releases the codec + its I2C bus (no-op if none)
        inited_ = false;
        filled_ = 0;
        // Publish silence: latestFrame() hands frame_ to consumers whenever this is
        // the active mic, independent of inited_. Without this, a mic that worked
        // and then lost its bus (a failed reinit after a pin edit, or release)
        // would leave the last real frame frozen on the LEDs instead of going dark.
        frame_ = AudioFrame{};
        // The ANALYSIS history goes with it, so a restarted source begins from a DEFINED state
        // rather than the old source's last block: flux is a difference against the previous
        // block, and the onset detector carries a running mean. The zeroed frame above is what
        // keeps the first block after a restart silent (measured against zeros it would otherwise
        // read as a full-scale rise), and this is what keeps the second one honest.
        std::memset(prevBands_, 0, sizeof(prevBands_));
        onset_ = OnsetDetector{};
    }

    // --- WLED audio sync (guarded: only compiled where platform::hasNetwork) ---

    /// Reset the sync socket to the current mode. Called from setup()/prepare()
    /// so a `sync` control change applies live (no reboot). This only CLOSES the socket
    /// and records the mode, it never opens one, because setup() runs at boot before
    /// NetworkModule brings an interface up, and any lwip socket call before then asserts
    /// (the core mutex is still null). The actual open() is deferred to syncEnsureSocket(),
    /// which runs from the tick path once platform::networkReady() is true.
    void syncReinit() {
        if constexpr (!platform::hasNetwork) return;
        syncSock_.close();                 // syncEnsureSocket() re-opens per mode when the net is up
        syncOpen_ = false;
        lastSyncOpenFailMs_ = 0;           // a mode change retries bring-up immediately (no stale backoff)
        lastSyncRecv_ = 0;
        std::memset(syncPeer_, 0, sizeof(syncPeer_));
        const uint8_t s = sync();
        // Only when there IS a socket to wait for. With sync off this cleared the line
        // unconditionally, wiping a mic diagnosis ("check sdPin") that Local mode had just set: the
        // user then saw an empty status for a mic that is still not working.
        // With sync OFF, clear only what this function itself put there: a mic diagnosis from Local
        // mode has to survive, and clearing unconditionally wiped it, so the user saw an empty line
        // for a mic that still was not working.
        if (s == 1)      setStatus("send: waiting for network");
        else if (s == 2) setStatus("receive: waiting for network");
        else if (const char* cur = status();
                 cur && (std::strstr(cur, "waiting for network") || std::strstr(cur, "socket failed")
                         || std::strstr(cur, "bind failed") || std::strstr(cur, "from ")
                         || std::strstr(cur, "listening on")))
            setStatus("");
    }

    /// Lazily open the sync socket for the current mode, once the network stack is up.
    /// Idempotent: opens exactly once per mode (syncOpen_ latch), re-armed by syncReinit()
    /// on a mode change. Returns true when the socket is ready to use this tick. Off is a
    /// no-op (socket stays closed, zero overhead). Send connects to the WLED multicast group;
    /// Receive binds the port and joins that group. Mirrors NetworkSendDriver/
    /// NetworkReceiveEffect, but deferred past boot so a boot-present AudioService can't touch
    /// lwip before it exists.
    bool syncEnsureSocket() {
        if constexpr (!platform::hasNetwork) return false;
        const uint8_t s = sync();
        if (s == 0) return false;
        if (syncOpen_) return true;
        if (!platform::networkReady()) return false;   // interface not up yet, try again next tick
        // Back off between failed bring-ups: tick() runs every tick, so without this a
        // persistent open/bind failure (e.g. the port is busy) would retry, one socket()
        // syscall per tick, dozens of times a second. lastSyncOpenFailMs_ stamps the last
        // failure; hold off until kSyncOpenRetryMs has passed (same throttle form as syncSend).
        const uint32_t now = platform::millis();
        if (lastSyncOpenFailMs_ != 0 && now - lastSyncOpenFailMs_ < kSyncOpenRetryMs) return false;
        if (s == 1) {                      // send → the WLED multicast group (configurable port)
            char grp[16]; formatDottedQuad(grp, kSyncMulticastAddr_);
            if (syncSock_.open() && syncSock_.connect(grp, syncPort)) {
                syncOpen_ = true;
                setStatus("sending");
            } else {
                syncSock_.close();
                setStatus("send: socket failed", Severity::Error);
            }
        } else {                           // receive → bind the port, then JOIN the group
            // The join is what makes a multicast datagram reach this socket at all: binding the
            // right port is not enough, the stack drops the group's traffic without a membership.
            char grp[16]; formatDottedQuad(grp, kSyncMulticastAddr_);
            if (syncSock_.open() && syncSock_.bind(syncPort) && syncSock_.joinMulticast(grp)) {
                syncOpen_ = true;
                setStatus("listening");
            } else {
                syncSock_.close();
                setStatus("receive: bind failed", Severity::Error);
            }
        }
        // Stamp a failure (or clear the timer on success). now==0 is nudged to 1 so the
        // "!=0 means a failure is pending" sentinel holds even at millis()==0.
        lastSyncOpenFailMs_ = syncOpen_ ? 0 : (now == 0 ? 1 : now);
        return syncOpen_;
    }

    /// Broadcast the current frame_ as a WLED v2 packet, throttled to ~40/s. Called
    /// from tick() in Send mode. Cheap: builds a 44-byte packet, one non-blocking sendTo.
    void syncSend() {
        if constexpr (!platform::hasNetwork) return;
        const uint32_t now = platform::millis();
        if (now - lastSyncSend_ < kSyncSendIntervalMs) return;
        lastSyncSend_ = now;
        // samplePeak is a LATCHED beat flag, the way WLED sends it: a beat seen between two
        // transmits is reported once and then cleared, with a refractory window so one loud
        // passage cannot set it on every packet. Testing the level per packet instead (what this
        // did) flagged ~24% of packets where WLED flags ~4%, so a receiver's beat-driven effect
        // fired continuously.
        const bool peak = syncPeakLatched_;
        syncPeakLatched_ = false;
        uint8_t pkt[WLED_SYNC_PACKET_SIZE];
        buildWledAudioSync(pkt, frame_, peak);
        syncSock_.sendTo(pkt, WLED_SYNC_PACKET_SIZE);
        syncSendCount_++;
    }

    /// Drain the sync socket (bounded, non-blocking) in Receive mode. A valid v2 packet
    /// overwrites frame_ and stamps lastSyncRecv_. Returns true while a peer's audio is
    /// FRESH (within kSyncFallbackMs) so tick() skips the local mic analysis, false once
    /// the peer goes quiet, letting the local mic resume (auto-blend).
    bool syncReceive() {
        if constexpr (!platform::hasNetwork) return false;
        uint8_t pkt[WLED_SYNC_PACKET_SIZE + 8];   // a little slack over the 44-byte v2
        uint8_t srcIp[4] = {};
        for (int i = 0; i < kSyncMaxRecvPerTick; i++) {
            const int n = syncSock_.recvFrom(pkt, sizeof(pkt), srcIp);
            if (n <= 0) break;                     // -1 = nothing pending
            AudioFrame rf;
            if (parseWledAudioSync(pkt, static_cast<size_t>(n), rf)) {
                // The packet carries RAW bands; the ballistic is ours and lives across packets.
                // A whole-frame copy would zero it forty times a second, so the smoothed state is
                // carried over the copy and then advanced by this packet's bands, exactly as the
                // mic path advances it by a block.
                uint8_t keep[16];
                std::memcpy(keep, frame_.bandsSmoothed, sizeof(keep));
                frame_ = rf;                       // received audio drives the effects
                std::memcpy(frame_.bandsSmoothed, keep, sizeof(keep));
                finishBands();
                lastSyncRecv_ = platform::millis();
                // Whose audio this is. A receiver with no peer named looks identical to one taking
                // the wrong source, and on a multi-device rig that is the question being asked.
                std::memcpy(syncPeer_, srcIp, sizeof(syncPeer_));
                // Feed the peer level into the same 1 s peak window the local mic uses, so the
                // "level RMS" read-out (tick1s → levelStr_) reflects received audio too, otherwise
                // it freezes at the last local value while a peer is driving the effects.
                if (frame_.level > levelPeak_)
                    levelPeak_ = static_cast<uint8_t>(frame_.level > 255 ? 255 : frame_.level);
            }
            // else: a v1 / foreign packet, ignore, keep draining.
        }
        // Fresh received audio → skip local mic. Stale (peer quiet) → fall through.
        return lastSyncRecv_ != 0
            && (platform::millis() - lastSyncRecv_) < kSyncFallbackMs;
    }

    static constexpr uint16_t kSyncPeakMargin = 8;   // level over smoothed = a beat (samplePeak hint)
    // WLED gates its own peak detection on `millis() - timeOfPeak > 80`, so one loud passage
    // reports a few beats rather than a continuous run of them.
    static constexpr uint32_t kSyncPeakRefractoryMs = 80;
    // The IP MULTICAST ADDRESS WLED audio sync uses: a network-layer destination, NOT a "sync
    // group" in the WLED-feature sense and unrelated to any device grouping projectMM adds later
    // (that is free to work however it likes: this only decides where the datagrams are sent).
    // WLED's usermod both sends and receives on this address
    // (audio_reactive.cpp: beginMulticast(IPAddress(239,0,0,1), port) + beginMulticastPacket),
    // never on broadcast, so a broadcast sender is inaudible to WLED and a plain bound receiver
    // never hears WLED. Multicast is also the better neighbour: only members are woken, where a
    // broadcast at ~40/s makes every device on the LAN parse and discard a packet.
    static constexpr uint8_t kSyncMulticastAddr_[4] = {239, 0, 0, 1};
};

} // namespace mm
