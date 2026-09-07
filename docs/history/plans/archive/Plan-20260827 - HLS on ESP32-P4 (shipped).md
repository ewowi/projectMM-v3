# Plan: HLS on ESP32-P4 — hardware H.264 behind the same HlsDriver

## Context

The P4 has a hardware H.264 ENCODER (no decoder, which is irrelevant: HLS only encodes), so a
P4 can stream its wall to a TV with no desktop in the loop. The design goal, confirmed with
the PO: the EXISTING HlsDriver stays the one module; only the platform side gains a P4
implementation of the encoder seam. Espressif's `esp_h264` managed component drives the
hardware encoder; the HLS packaging (MPEG-TS muxer) and segment store are ours.

## Two structural changes the exploration proved necessary

1. **The seam carries ffmpeg CLI strings today** (buildArgs flattens geometry/fps/bitrate into
   argv; a P4 impl would have to string-parse `-s`/`-r`/`-b:v`). Refactor to structured
   params: `struct EncoderConfig { uint16_t w, h; uint8_t fps; uint16_t bitrateKbit;
   const char* encoderName; const char* outDir; }` and
   `bool encoderStart(const EncoderConfig&)`. The desktop impl builds its ffmpeg argv FROM
   the struct (argv assembly moves from the driver into platform_desktop, where ffmpeg
   knowledge belongs anyway); the P4 impl consumes the numbers directly. The argv pin test
   moves to the desktop side of the seam (`encoderTestArgs()` unchanged); the driver's
   buildArgs dies. The `encoder` Select stays: on P4 the platform ignores the name (one
   hardware encoder) and the control hides via a new capability
   `platform::hasEncoderChoice` (desktop true, P4 false).
2. **`/hls/` serving is fs-only** (serveHlsFile → streamFsFile → fsSize/fsReadAt). P4
   segments live in PSRAM, not LittleFS (flash wear at one segment/second). Chosen hook: a
   RAM branch INSIDE serveHlsFile before the fs fallthrough (the serveFile disk-then-embedded
   precedent, HttpServerModule.cpp:873): new seam
   `bool hlsSegment(const char* name, const uint8_t** data, size_t* len)` — desktop returns
   false (fs path unchanged), P4 serves from the segment ring. Serve with the chunked
   error-checked loop (streamFsFile's shape), not serveFile's single write.

## The P4 platform implementation (new src/platform/esp32/platform_esp32_h264.cpp)

Added to esp32/main/CMakeLists.txt SRCS (the one-file-per-seam convention). Everything inside
`#if CONFIG_MM_HLS` (a new Kconfig symbol, see gating).

- **Pipeline**: encoderWrite copies the RGB frame into a PSRAM slot ring (the desktop's
  3-slot reuse-ring shape); an `mmH264` pinned task (spawnPinnedTask, 8 KB, priority 5,
  core 1 — the mmEncode/urlOta conventions, WDT-subscribed per platform_esp32_worker.cpp's
  contract) converts RGB→YUV420 (CPU; sub-ms at 256²) and feeds `esp_h264` hardware encode.
- **Muxing**: our MPEG-TS muxer (~300-500 lines, its own header `platform_esp32_h264_ts.h`
  or folded in): PAT/PMT + H.264-in-PES from the encoder's Annex-B NALs, 188-byte packets,
  cut on keyframes (GOP = fps = 1 s segments, the driver's existing contract).
- **Segment ring**: N=8 segments in PSRAM (`platform::alloc`, PSRAM-first; P4-NANO has
  32 MB; ~1 s at 2-4 Mbit ≈ 250-500 KB → ring ≈ 2-4 MB) + a generated m3u8 string, exposed
  via the `hlsSegment` seam. `/.hls` on-disk never exists on P4; HlsDriver's
  fsMkdir/clearSegments become no-ops behind `platform::fsMkdir` returning true (verify) or
  get a `hasFsSegments` guard — pick during implementation, smallest wins.
- **Lifecycle**: encoderStart allocates ring + esp_h264 session; encoderRunning = session
  alive; encoderStop joins the task (stopPinnedTask, bounded) and frees; warm-up handled by
  the driver as today (harmless).

## Gating

- `hasHls` in esp32/platform_config.h becomes SOC-derived per the file's own rule
  (`CONFIG_MM_HLS`-mirrored `#define` like the MM_HEAVY_COMPUTE precedent).
- `esp_h264` dependency in esp32/main/idf_component.yml gated
  `$CONFIG{MM_HLS} == True` (the documented Kconfig-gate idiom — NOT a bare target gate,
  which would land it in all four P4 images; the ip101 comment records that ungated deps
  break other chips' solves). `MM_HLS` declared in esp32/main/Kconfig.projbuild, default y
  only on P4 targets; rev1 and rev3 both get it (the two-generation duplication rule).
- IDF pin v6.1-rc1: verify esp_h264's compatibility first (step 1 below); if it needs older
  IDF, the whole plan gates on that finding.

## Steps

1. **Spike (half day, gates everything)**: add esp_h264 to a P4 build, encode ONE synthetic
   frame on the bench P4 (.139), verify NALs come out under IDF v6.1-rc1. No driver wiring.
2. Seam refactor to EncoderConfig (desktop argv moves into platform_desktop; driver's
   buildArgs deleted; argv test relocated; hasEncoderChoice hides the Select on P4).
3. TS muxer + unit tests (host-buildable pure code: feed canned NALs, assert packet
   structure, PAT/PMT, continuity counters — testable on desktop, no P4 needed).
4. P4 pipeline file (ring, task, RGB→YUV, esp_h264 wiring) + hlsSegment seam + the RAM
   branch in serveHlsFile.
5. hasHls gating + Kconfig + component manifest (rev1 + rev3 fragments).
6. Docs: drivers.md HLS card gains the P4 paragraph; the spec's ESP32-out-of-scope line
   updated; backlog entry closed.

## Tests

- Host: TS muxer unit tests (the substance); EncoderConfig seam pin replaces the argv pin
  at driver level; existing HlsDriver tests unchanged (Record seam untouched).
- Bench (the real gate): P4 .139 streams to VLC; ffprobe confirms h264 at grid size;
  soak + kill/restart; the PO's TV.

## Verification

Desktop build + ctest green (seam refactor must not disturb the desktop path: re-verify the
live macOS stream after step 2). P4: esp32p4rev1-eth-wifi on the bench board at .139, url
control shows the P4's address, VLC plays, uptime stable through a 10-minute soak.

## Risks

- esp_h264 vs IDF v6.1-rc1 compatibility is unproven: the step-1 spike settles it before
  anything else is built.
- P4 WiFi throughput (hosted C6 link) may cap bitrate: Ethernet is the primary path, WiFi
  best-effort.
- Encoder memory appetite (esp_h264 internal buffers) on top of the app: measured in the
  spike; the ring is PSRAM so main heap stays untouched.
- The rev3 images stay untested hardware-wise (no rev3 board on the bench) — same caveat
  they already carry.
