# Plan: HlsDriver — pixel-exact HLS streaming via ffmpeg (+ GridLayout 4K number fields)

## Context

The PO wants to watch effects pixel-exact (1:1, no scaling) on a TV, up to 4K transport. Spec:
`docs/backlog/hls-driver-spec.md` (deleted when the driver shipped, per the backlog's drain rule). Decisions already made with
the PO: pipe raw frames to a spawned **ffmpeg** (runtime dependency like Npcap/NDI, never
vendored; one implementation for every desktop OS + Pi), H.264 + HLS served by our own HTTP
server, 2-5 s live-tuned latency, ESP32 out of scope. Rides along: GridLayout `width`/`height`
become plain number inputs with 4K bounds (3840 x 2160).

NdiDriver is the template throughout: capability-gated registration, source-buffer frame pull,
geometry from the layer, fps pacing, warning-status on missing runtime, record-mode test seam
(explored: NdiDriver.h, main.cpp:266, platform_desktop.cpp:2148-2202, unit_NdiDriver.cpp).

## Two spec deviations (cheaper v1, noted in the spec when implementing)

1. **mDNS advert deferred.** Desktop has NO mDNS implementation today (platform_desktop.cpp:1315
   stubs return false); Apple TV does not browse DNS-SD anyway. The read-only `url` control +
   docs cover discovery. Building Bonjour/Avahi from scratch is its own item.
2. **libx264 first, hardware encoders later.** At today's practical grid sizes (<= 512 x 512)
   `libx264 -preset veryfast -tune zerolatency` costs trivial CPU on any desktop. Hardware
   encoder selection (videotoolbox/MF/VAAPI) becomes worthwhile with the render-scaling item.

## Design

**New platform seam — process-with-stdin-pipe** (nothing exists today; the only precedent is a
one-off `std::system` in main_desktop.cpp:96). Declared unconditionally in platform.h under a
banner section (the NDI/Npcap convention), desktop-implemented, `hasHls` constant per
platform_config (desktop true, esp32 false):

- `bool encoderStart(const char* const argv[], const char* outDir)` — spawn ffmpeg (PATH
  discovery), stdin piped non-blocking (POSIX: posix_spawn + O_NONBLOCK; Windows:
  CreateProcess + named pipe, overlapped or PeekNamedPipe-guarded writes)
- `int encoderWrite(const uint8_t* data, size_t len)` — full frame write; returns written /
  0 = would-block (caller drops the frame) / -1 = process dead
- `bool encoderRunning()`, `void encoderStop()` (TERM, wait briefly, KILL)
- Test seam (`#ifndef ESP_PLATFORM`, mirrors NdiTestMode): `setTestEncoderMode(Record)` makes
  encoderStart a no-op recorder; `encoderTestFrameCount/Width.../Data`, `encoderTestArgs()`.

**HlsDriver** (src/light/drivers/HlsDriver.h, ~NdiDriver-sized):

- Registration in main.cpp: `if constexpr (mm::platform::hasHls)` — include unconditional,
  exactly the hasNdi block at main.cpp:266 (the discarded branch must parse; NDI proves the
  pattern links on all ESP32 builds with no stubs).
- `defineDriverControls()`: `targetFps` (1..120, default 30), `bitrate` kbit (500..40000,
  default 8000, setNumberField), read-only `status`, read-only text `url`
  (`http://<ip>:<port>/hls/stream.m3u8`, built from NetworkModule's address + HttpServer port).
- `prepare()`: geometry from `layer_->physicalWidth()/physicalHeight()` (NdiDriver.h:66),
  size the tight-RGB ScratchBuffer, build the ffmpeg argv, `encoderStart`. ffmpeg missing →
  `setStatus("ffmpeg not found - see the docs", Severity::Warning)` and stay closed
  (NdiDriver.h:69 pattern). Args:
  `ffmpeg -f rawvideo -pix_fmt rgb24 -s WxH -r F -i - -c:v libx264 -preset veryfast
  -tune zerolatency -g 2F -b:v Nk -f hls -hls_time 1 -hls_list_size 6
  -hls_flags delete_segments <segdir>/stream.m3u8`
- `tick()`: fps pacing (the `lastSendTime_` pattern, NdiDriver.h:107), pack tight RGB with
  correction (reuse NdiDriver.h:130-143 approach), `encoderWrite`; would-block → `dropped_++`
  (surfaced in `status` from tick1s); dead → restart with backoff (3 tries, then error status).
- `release()`: `encoderStop()`, delete the segments dir, chain `DriverBase::release()`.
- `affectsPrepare`: targetFps, bitrate (new encode geometry → respawn).

**Segments location + serving**: segments live INSIDE the fs mount at `/.hls/` (no new
absolute-path read seam; desktop disk). HttpServerModule gets an `/hls/` prefix route
(the `/api/modules/` strncmp pattern, HttpServerModule.cpp:272): maps `/hls/x` → fs path
`/.hls/x`, MIME by extension (`application/vnd.apple.mpegurl` for .m3u8, `video/mp2t` for .ts),
`Cache-Control: no-cache`, streamed in chunks via the existing fsReadAt loop
(serveFileContents shape, HttpServerModule.cpp:637). Segment size at 1 s / 8 Mbit ≈ 1 MB;
served synchronously like today's file downloads — acceptable for v1, and the
drainPreviewSend/writeSome pattern is the named follow-up if the per-segment render stall
proves visible in the stream itself.

**Backup interaction**: `/.hls/` is transient binary output; the backup walker (collectFiles in
src/ui/migrate.js) gets a one-line transient-dir exclusion so backups don't fill with skipped
.ts noise (and the bookmarklet mirrors it).

**GridLayout** (src/light/layouts/GridLayout.h:24): `width` max 3840, `height` max 2160, both
`setNumberField(count()-1)` (the Control.h:620 idiom, NetworkModule.h:374 precedent); `depth`
and `serpentine` unchanged. `lengthType` is int16_t — 3840 fits.

## Steps

1. Platform seam: platform.h declarations + `hasHls` in both platform_configs; desktop
   implementation (POSIX first, then the Windows branch) + record-mode test seam.
2. HlsDriver.h + main.cpp registration.
3. HttpServerModule `/hls/` route with MIME + no-cache.
4. GridLayout number fields + 4K bounds.
5. Backup walker `/.hls/` exclusion (migrate.js + backup-snippet.js).
6. Tests (below), docs: drivers.md card (latency expectation, install-ffmpeg line, Apple TV:
   VLC or Safari-AirPlay hand-off), building.md runtime-dependency line, spec updated with the
   two deviations then renamed per shipped convention at merge.

## Tests

- unit_HlsDriver.cpp (mirrors unit_NdiDriver.cpp: seam guard RAII, Wall fixture, virtual
  clock): argv builder (geometry/fps/bitrate exact), pacing honors targetFps, would-block
  drops without blocking the tick, ffmpeg-absent → warning status and quiet ticks,
  release stops the encoder and removes /.hls/.
- unit test for the `/hls/` MIME/no-cache route (path mapping + headers, no socket needed if
  factored like parseFilePath; else via the serve path on desktop).
- JS: backup excludes /.hls/ (one case in backup-bundle.test.mjs).
- Host-only integration (non-CI-critical): spawn a fake-ffmpeg python script that drains stdin
  and writes a playlist; assert real spawn + non-blocking pipe + kill/restart on macOS.

## Verification

Desktop build zero warnings; ctest + JS suites green. Live (PO's eyes are the measurement):
512x512 grid → VLC on Mac and Apple TV (VLC-tvOS URL paste, and Safari AirPlay hand-off),
single-pixel test pattern confirms 1:1, measure glass-to-glass latency (expect 2-5 s), kill
ffmpeg mid-stream and watch status + recovery, disable/enable the driver live.

## Risks

- Windows pipe non-blocking semantics are the fiddliest part; POSIX lands first, Windows is
  its own step with the same seam contract (Windows testers post-release, as with audio).
- Per-segment serving stalls the render ~10-50 ms every second on active viewers; if visible,
  the writeSome drain pattern (already proven by preview) is the named fix.
- ffmpeg arg drift across versions: pinned by the argv-builder unit test and a doc line naming
  the minimum ffmpeg (5.x).
