# Plan: Desktop live audio capture into AudioService

## Context

Audio-reactive effects are silent on desktop builds: `AudioService::tick()` returns before reading a
mic because desktop has no I2S peripheral (`if constexpr (!platform::hasI2sMic) return;`,
AudioService.h:344). The feature: desktop builds (macOS/Windows/Linux) capture live audio from a
user-selectable OS input device (built-in mic by default; loopback devices such as BlackHole appear
when installed) and feed the existing DC-blocker → RMS → FFT → 16-bands pipeline unchanged. Because
the existing "send audio" broadcast runs off the locally-analyzed `frame_` and is gated only on
`hasNetwork` (verified: AudioService.h:328-333), a desktop then also works as a WLED audio-sync
SOURCE for a fleet of boards — a required outcome, pinned by test and bench.

PO decisions: selectable input device; all three desktop OSes in v1; backend = vendored
**miniaudio** single header (approved new precedent; public domain/MIT-0, no link deps).

## Design (settled)

- **Vendored header**: `src/platform/desktop/vendor/miniaudio.h`, untouched upstream.
  `MINIAUDIO_IMPLEMENTATION` compiles once in new `src/platform/desktop/platform_desktop_audio.cpp`
  with `MA_NO_DECODING/ENCODING/GENERATION/RESOURCE_MANAGER/NODE_GRAPH/ENGINE`, the include wrapped
  in diagnostic-suppression pragmas (our own code in the TU stays -Wall/-Werror clean). Links: none
  on macOS/Windows (miniaudio runtime-links CoreAudio/WASAPI); `dl` on Linux. Vendor exclusions
  added to check_clang_tidy.py `VENDORED`, lizard/repo-health/KPI LOC, clang-format globs.
- **Seam**: keep `audioMicRead`/`audioMicDeinit`/`AudioMicHandle` (same 24-bit-left-justified mono
  int32 contract). Add to platform.h:
  `size_t audioCaptureDevices(const char* const** optionsOut)` (platform-owned stable strings,
  entry 0 = "default") and `bool audioCaptureInit(AudioMicHandle&, uint8_t deviceIndex, uint32_t sampleRate)`.
  Gates: keep `hasI2sMic` (pin-wired I2S, gates pin controls); add `hasAudioCapture`
  (desktop true / esp32 false); derive `constexpr bool hasAudioInput = hasI2sMic || hasAudioCapture;`
  and switch AudioService's three gates (tick :344, reinit :543, deinit :596) to it.
- **Device control**: `addSelect("device", device, options, n)` under `if constexpr (hasAudioCapture)`,
  options re-enumerated each defineControls (hot-plug), in `affectsPrepare()` (live re-init). Pin
  controls move under `if constexpr (hasI2sMic)`. Persistence stays by index (the Select writer's
  contract); entry 0 "default" is order-stable; documented hazard, label-persistence stays open.
- **Data path**: miniaudio capture as s32/mono at `sampleRate()` (miniaudio resamples; full-scale
  s32 IS the seam's 24-bit<<8 regime — zero conversion). Callback thread → polled read via a
  textbook Lamport SPSC ring, new `src/core/SpscRing.h` (core owns the hard construct;
  atomic head/tail acquire/release, power-of-two, capacity 4096 samples ≈ 186 ms). Drop-newest on
  overflow (drop-oldest would add a second writer to the consumer index, breaking SPSC).
- **Desktop FFT**: replace the naive O(n²) DFT body (test-grade per its own comment) with a textbook
  iterative radix-2 Cooley-Tukey (~40 lines, identical output semantics). `unit_AudioBands` pins
  behavior and passes unchanged; a new test pins numerical equivalence vs a test-local DFT reference.
- **ESP32**: zero behavior/flash delta — miniaudio never enters the build; two GC'd stubs +
  `hasAudioCapture=false` + one uint8_t member. Verified via KPI.
- **Send audio on desktop**: no code needed beyond the gate flip (syncSend precedes the local path
  and broadcasts frame_). Pinned, not assumed (tests + bench below).

## Steps (each builds + tests green)

Status 2026-08-27: steps 1-6 implemented and green locally; remaining before shipped: the
Windows/Linux CI compile proof (first push), the PO fleet test, and the Windows tester's
run after the merge.

1. **Vendor + build plumbing** (done; miniaudio 0.11.25 pinned, compiles fully warning-clean under local clang after two suppression rounds; macOS/CI-linux/windows proof on push): miniaudio.h, the implementation TU (defines+pragmas only),
   CMake (`platform_desktop_audio.cpp`, Linux `dl`), gate exclusions
   (check_clang_tidy VENDORED tuple, check_lizard, repo_health, collect_kpi, clang-format glob —
   verify each script's mechanism before editing). CI on all three OSes proves warning-cleanliness
   before anything depends on it.
2. **SpscRing** (done; 3 cases incl. a real two-thread 200k-element run): `src/core/SpscRing.h` + `test/unit/core/unit_SpscRing.cpp` (FIFO across wrap,
   drop-newest semantics, bounded two-thread run) + test/CMakeLists.txt.
3. **Platform seam + capture** (done; enumeration verified against the real device list, lifecycle test tolerant of host permission): platform_config.h flags (both platforms), platform.h declarations +
   `hasAudioInput`, desktop audio block moves from platform_desktop.cpp (~:2022-2057) into
   platform_desktop_audio.cpp (lazy ma_context, enumeration with static name cache,
   capture device → SpscRing<int32_t,4096>, ring-pop audioMicRead, stop/uninit deinit),
   ESP32 stubs in platform_esp32_i2s.cpp. Test `unit_AudioCapture.cpp`: enumeration ≥1 with
   "default" at 0; init/read/deinit lifecycle (tolerant of init-failure on locked-down hosts —
   miniaudio's null backend makes success the CI norm); bad index fails cleanly; double-deinit safe.
4. **Radix-2 FFT** (done; 766-assertion equivalence vs the DFT reference) replacing the DFT body + `unit_platform_audiofft.cpp` (equivalence vs local DFT
   reference on random vectors + sine; silence → zeros).
5. **AudioService wiring** (done; all 23 scenarios pass, the Audio scenario now does a live capture reinit mid-render; the fleet-source coexistence case pins send+capture in one tick) (one step with its tests): gates → `hasAudioInput`; pins block under
   `hasI2sMic`; `device` member + Select + affectsPrepare; reinit's capture branch with status
   "capture init failed — pick another device"; deinit; tick1s wire-diagnosis gated to `hasI2sMic`;
   class `///` refreshed ("inert on desktop" no longer true; name the loopback/BlackHole use AND
   the desktop-as-sync-source use). Update unit_AudioService / unit_AudioService_sync status
   expectations; scenario_Audio_mutation.json pin steps → mode/device steps; regenerate test docs.
   **Send-audio pin**: extend unit_AudioService_sync (real localhost UDP harness exists) with a
   case proving Local+send broadcasts a frame on a `hasAudioCapture` build.
6. **Docs + verification** (done incl. the NSMicrophoneUsageDescription packaging key and a bench-learned doc line: loopback audio needs single-digit gain, the mic-tuned default clips everything to max): services.md #audio (`device` bullet, pins "(Local, I2S targets)",
   desktop-as-source sentence); audio-dsp-roadmap source-seam line; plan file per process.
   **macOS packaging**: `NSMicrophoneUsageDescription` in the .app Info.plist
   (moondeck/ci/package_desktop.py) — without it macOS kills the process at first capture.
   Optional PO call: a short ADR for the first vendored runtime header.

## Verification

- ctest (new: SpscRing, AudioCapture, audiofft; updated: AudioService, sync) + scenarios + spec check — all green.
- Desktop bench: PO-verified 2026-08-27 — device dropdown listed the Mac's real inputs (BlackHole
  2ch included), mic capture reacted in AudioSpectrum, BlackHole loopback followed Spotify via a
  Multi-Output Device, and the GEQ3D stillness discriminated to an audio-shape/gain topic
  (Simulate sweep moved it), parked.
- ESP32 zero delta: verified, -112 bytes (the pins block now compiles out where hasI2sMic is
  false). Future size checks read the repo-health delta instead of A/B builds (PO rule).
- Fleet test: verified 2026-08-27 on the bench: the desktop (Local, BlackHole, send audio on,
  "sending") drove the S3 N8R8 at .103 ("receiving", level tracking the desktop's in lockstep).
- Still open: the Windows tester's run after the merge.
- Desktop bench (PO): run `build/projectMM` on macOS, pick the mic in the Audio card device
  dropdown, see AudioSpectrum/GEQ effects react; install BlackHole and see it appear + react to
  played music.
- **Fleet test (PO)**: desktop Local + "send audio" on; the Olimex's AudioService in Receive mode;
  board effects follow the desktop's captured audio.
- ESP32: KPI/footprint confirms ~0 flash delta; the three-variant gate builds.

## Pre-merge notes

- The Reviewer's 8 findings were fixed on the branch (the I2S wire-diagnosis gate had silently
  missed the file in an earlier edit and is now in with a pinning test; the ESP32 capture stubs
  moved outside the SOC_I2S split; the scenario's device step is optional for board targets;
  check_prose gained the vendor exemption; em-dash sweep; SpscRing comment corrected; the
  roadmap's dated shipped line deleted).
- scenario_peripheral_grid_sweep's desktop tick bound widened 34 to 50 us: the scenario runner
  re-records its observation envelope during gate runs, and those ran beside parallel cold GCC
  builds on this host; a contended-host measurement, not an audio-branch regression (the
  scenario contains no Audio module, and miniaudio is inert until a device opens).

## Risks

1. macOS TCC mic permission: needs the Info.plist key in packaging; denied permission must degrade
   to a status line, not a crash; first terminal run prompts.
2. Warning-clean vendored compile under -Werror//WX on three compilers (step 1 burns this down first).
3. Headless CI rests on miniaudio's null-backend fallback; tests written tolerant, seam lands
   before AudioService depends on it so failures surface isolated.
