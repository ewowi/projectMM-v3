#pragma once

#include <atomic>
#include <cstdint>
#include <cstddef>
#include "platform_config.h"  // hasOta / hasPsram / …: flags this header's contract refers to

// The render path must not allocate or block (architecture.md § Hot path discipline). Clang 20+
// checks that TRANSITIVELY under -Wfunction-effects: the attribute is inherited by overrides, so
// marking the three tick methods here covers every module's tick and everything it calls, which a
// regex over source text can never do: it sees a tick body, not what its callees reach.
//
// On GCC it expands to `noexcept` alone: that toolchain has neither the attribute nor the warning
// (so the effect check is desktop-only), but the exception contract still holds. It builds with
// -Werror, so a bare [[clang::nonblocking]] there is a build break (-Wattributes). Same shape as
// MM_PRINTF_FORMAT in JsonSink.h. That makes this a DESKTOP-side check, which loses nothing: every
// tick method: modules, effects, and the LED drivers: compiles on desktop. src/platform/esp32/
// has no tick methods at all; it is free functions the tick path calls INTO, and those are checked
// through their call sites.
// Feature-tested, not version-inferred: Apple Clang carries its own version line, so a
// `__clang_major__ >= 20` check reports true on toolchains that predate the attribute: the CI
// macos-14 runner is exactly that case. __has_cpp_attribute asks the compiler directly.
#if defined(__clang__) && defined(__has_cpp_attribute) && __has_cpp_attribute(clang::nonblocking)
  // noexcept is part of the contract, not decoration: clang warns
  // (-Wperf-constraint-implies-noexcept) if a nonblocking function can throw, because
  // unwinding allocates. Folded in here so the two never drift apart.
  #define MM_NONBLOCKING noexcept [[clang::nonblocking]]
#else
  #define MM_NONBLOCKING noexcept
#endif

namespace mm::platform {

uint32_t millis() MM_NONBLOCKING;

/// An opaque identity for the calling thread/task, stable for its lifetime and distinct between
/// concurrent ones. Zero is never returned, so a caller can use it as "no thread recorded".
///
/// Exists because C++ `thread_local` is NOT usable on the ESP32: the compiler reaches TLS through
/// the THREADPTR special register, and a FreeRTOS task that was not created with TLS initialized
/// has THREADPTR = 0: so the access dereferences a small offset from null (0xfffffff0 was the
/// measured faulting address) and dies inside the exception handler as a Double exception. This is
/// the portable seam for "which thread am I", used where per-thread state is genuinely needed.
uintptr_t currentThreadId() MM_NONBLOCKING;
uint32_t micros() MM_NONBLOCKING;

// Test-only override: when set to non-zero, millis() returns this value instead
// of reading the platform clock. Production code never calls this; tests use it
// to drive virtual time deterministically (replaces the wall-clock delayMs in
// animation tests). Pass 0 to restore real-clock behavior: tests must reset
// in release so cases stay independent. ESP32 honours the override too so a
// scenario-tests run on real hardware can still freeze time if needed.
void setTestNowMs(uint32_t ms);

// Force the next UdpSocket::bind() calls to fail, so a test can exercise a bind-failure path without
// depending on the OS to refuse a port. Production code never calls this. The alternative: hog the
// port with a second socket: is NOT portable: on Linux, SO_REUSEADDR on a UDP socket bound to
// INADDR_ANY *permits* the overlapping bind, so the hog succeeds and the failure never happens (this
// silently broke unit_AudioService_sync on Linux for as long as it existed; nothing caught it because
// CI did not compile the C++ tests until the sanitizer job). Nor is a privileged port reliable -
// modern macOS lets a non-root process bind port 80. Pass false to restore; tests must reset in
// release so cases stay independent, same contract as setTestNowMs.
void setTestBindFails(bool fail);

void* alloc(size_t bytes);
void free(void* ptr);

// Internal-RAM-only allocation: the mirror of alloc()'s PSRAM-first policy, for buffers a hot ISR READS.
// alloc() prefers PSRAM because most large buffers are touched from tasks where PSRAM latency amortizes;
// a buffer read per-byte inside an interrupt (the streaming ring's encode source) pays that latency
// hundreds of times per invocation and blows its deadline (measured: ~595 µs per slice refill with a
// PSRAM-resident source, against a 151 µs drain budget). Returns nullptr when internal RAM can't supply it -
// the CALLER decides the fallback (typically plain alloc(), accepting the slower PSRAM read over failing).
// Free with the ordinary free().
void* allocInternal(size_t bytes);

// True when the pointer resolves to external (PSRAM) memory: the standard residency probe (IDF's
// esp_ptr_external_ram). Diagnostic companion to allocInternal's internal-first-PSRAM-fallback pattern:
// the caller of that pattern cannot otherwise tell which way an allocation landed, and for buffers an
// ISR reads the difference is a measured 4-8x per-byte cost plus cache-contention exposure. Desktop has
// no PSRAM; always false.
bool ptrIsPsram(const void* p);

// CPU cycle counter (Xtensa CCOUNT / RISC-V mcycle; 0-based, wraps at 2^32: callers difference two
// reads). The standard fine-grained profiling primitive (ARM's DWT_CYCCNT, x86's rdtsc): a 1-instruction
// read, safe in ISRs, used by bench diagnostics to attribute hot-path cycles. Desktop returns a
// nanosecond-scaled clock so differences are still meaningful.
uint32_t cycleCount();

// Executable memory for JIT-emitted native code (MoonLive). Distinct from alloc()
// because code must live in memory the CPU can FETCH from, not just read/write:
// IRAM on ESP32 (MALLOC_CAP_EXEC), an mmap'd PROT_EXEC page on desktop. Returns
// nullptr when exec memory is exhausted: the caller degrades (status, runs dark),
// never crashes. freeExec takes the same size so a backend that needs it (munmap)
// has it; ESP32 ignores the size.
void* allocExec(size_t bytes);
void  freeExec(void* ptr, size_t bytes);

// Copy emitted code INTO an allocExec block, then make it executable. Separate from a
// plain memcpy because ESP32 IRAM is fetch-only on the instruction bus and writable
// only via 32-bit-aligned data-bus stores (a byte memcpy faults), and after writing,
// the instruction cache must be synced so the core fetches the fresh bytes, not stale
// cache. Both quirks live here, behind the platform line; the engine just hands over
// (dst-from-allocExec, src-bytes, len). On desktop this is a plain memcpy. `len` need
// not be a multiple of 4: the ESP32 path pads the final partial word.
void  writeExec(void* dst, const void* src, size_t len);

void yield();

// Which CPU core the caller runs on (0 or 1 on the S3; always 0 on single-core parts and desktop). The
// render loop is core 0; the multicore render/encode split runs a driver's tick on core 1: so a driver
// seeing core 1 here KNOWS the split is engaged and core 0 is the idle helper (xPortGetCoreID's role).
uint8_t currentCore();
// Upper bound on cores that run driver code concurrently: sizes per-CPU scratch (the textbook
// per-CPU-data pattern: one slice per core, no hot-path locking). A cap, not the exact count:
// single-core parts and desktop simply leave slice 1 unused.
inline constexpr uint8_t kMaxCores = 2;
// Pace the main render loop for one pass. The desktop parks the thread briefly; ESP32's yield()
// is already vTaskDelay(1), which lets the idle task run, so there it does nothing. Named for the
// job rather than for a duration so the two platforms can answer it differently: an unpaced loop
// spins a whole core on a desktop (reported from a Linux bench), while a sleep on ESP32 would come
// out of the render budget.
void pauseLoop();
void delayMs(uint32_t ms);  // blocking sleep; only use outside the hot path
void delayUs(uint32_t us);  // blocking busy-wait for sub-ms protocol gaps (e.g.
                            // the WS2812 inter-frame latch); fine for a few
                            // hundred µs, not a general-purpose sleep
size_t freeHeap();          // total free (internal + PSRAM if present)
size_t freeInternalHeap();  // internal RAM only (for stack/HTTP/WiFi reserve check)
size_t maxAllocBlock();     // largest contiguous block (any memory type: incl PSRAM)
size_t maxInternalAllocBlock(); // largest contiguous block in INTERNAL RAM only

// --- RTOS task introspection (TasksModule) --------------------------------------------------
// A fixed-size, allocation-free snapshot of the OS tasks, filled by the platform layer so no
// FreeRTOS type escapes src/platform/ (the platform-boundary rule). ESP32 fills it from
// uxTaskGetSystemState (the textbook RTOS-introspection call, needs CONFIG_FREERTOS_USE_TRACE_
// FACILITY); it uses a fixed static scratch (no heap) but briefly suspends the scheduler while it
// walks the task list: call it off the per-frame path (tick1s, once a second), not from tick().
// Desktop returns 0 (no RTOS). cpuPermille is 0..1000, or kTaskCpuUnmeasured when run-time-stats are
// compiled out (the cheap default): the caller shows a CPU% column only when it's a real number.
enum class TaskState : uint8_t { Running, Ready, Blocked, Suspended, Deleted, Invalid, Unknown };
constexpr uint32_t kTaskCpuUnmeasured = 0xFFFFFFFFu;
struct TaskInfo {
    char      name[16] = {};
    TaskState state = TaskState::Unknown;
    int8_t    core = -1;               // 0, 1, or -1 (no affinity)
    uint8_t   priority = 0;
    uint32_t  stackFreeBytes = 0;      // high-water-mark: min free stack ever seen for this task
    uint32_t  cpuPermille = kTaskCpuUnmeasured;  // 0..1000, or kTaskCpuUnmeasured (stats off)
};
// Fill `out` with up to `maxTasks` task rows; returns the count written. 0 on a target without
// an RTOS (desktop) or without the trace facility compiled in.
size_t taskSnapshot(TaskInfo* out, size_t maxTasks);
// Name of the task currently running on `core` (0 or 1); empty string if unavailable/single-core.
void currentTaskOnCore(int core, char* out, size_t cap);
// Name of the RTOS task that runs the render loop (Scheduler::tick): the task every MoonModule
// executes inside today. TasksModule nests the module rows under the matching `tasks` entry rather
// than hardcoding a task name. Empty on a target with no distinct render task (desktop).
const char* renderTaskName();
// Test-only (desktop): inject a canned task snapshot + render-task name so TasksModule's row/detail
// JSON + nesting predicate are testable on the host (no RTOS otherwise). `tasks` must outlive the use.
void setTestTaskSnapshot(const TaskInfo* tasks, size_t count, const char* renderTask);

// --- Pinned worker task + wake notification (render/encode multicore split) ------------------
// A minimal own-a-thread seam: spawn one function on a named task pinned to `core`, plus a
// single-slot wake notification. This is FreeRTOS's textbook lock-free pairing -
// xTaskCreatePinnedToCore + a direct-to-task notification (xTaskNotifyGive / ulTaskNotifyTake),
// which the RTOS documents as the lightweight replacement for a binary semaphore in a
// single-producer/single-consumer wake. The multicore pipeline (Drivers render↔encode split)
// is the first user; the async-ArtNet send wants the same primitive, so it lives in core.
// No FreeRTOS type escapes the header (opaque handle, same rule as RmtWs2812Handle). Desktop
// backs it with std::thread + a condition_variable so the handoff invariants are host-testable,
// even though the core-split itself is an ESP32-only capability.
struct WorkerTask { void* impl = nullptr; };
using WorkerFn = void(*)(void* user);
// Spawn `fn(user)` on a task named `name` with `stackBytes` stack, at `priority`, pinned to
// `core` (0 or 1; -1 = no affinity). Returns false if the task couldn't be created: the caller
// then runs the work inline (the allocate-and-degrade fallback). The spawned fn owns its loop and
// returns only after stopPinnedTask signals it.
bool spawnPinnedTask(WorkerTask& t, const char* name, WorkerFn fn, void* user,
                     size_t stackBytes, uint8_t priority, int core);
// Wake the task blocked in waitNotify (producer side; safe from any task on any core).
void notifyTask(WorkerTask& t);
// Block the spawned task until notifyTask fires or `timeoutMs` elapses; false on timeout (so the
// worker can service its own watchdog and re-check its stop flag). Called ONLY from inside the fn.
bool waitNotify(WorkerTask& t, uint32_t timeoutMs);
// Signal stop + wake, then block until the worker fn has returned and the task is torn down.
void stopPinnedTask(WorkerTask& t);
// Subscribe THIS task to the task watchdog (esp_task_wdt_add), so a wedge on it panics-and-reboots (the
// self-heal) instead of hanging silently. Called by each task that feeds the WDT before it starts ticking:
// the render loop, and the core-1 encode worker. The subscription is per-task (so is the feed and the flag
// behind it), which is why a task must subscribe itself rather than inherit another task's subscription.
// The sdkconfig has idle-task checking OFF (a saturated core is healthy, not a bug), so nothing is watched
// unless a task subscribes explicitly. No-op on desktop.
void taskWdtSubscribe();
// Unsubscribe THIS task from the watchdog (esp_task_wdt_delete) before it exits, so a torn-down task (the
// encode worker when the render split disengages) leaves no dangling WDT entry. No-op on desktop, and
// no-op on ESP32 if the task never subscribed.
void taskWdtUnsubscribe();
// Reset THIS task's watchdog (esp_task_wdt_reset): called each tick to feed the subscription above. No-op
// on desktop, and no-op on ESP32 if the task never subscribed.
void taskWdtReset();

// --- GPIO capability introspection (PinsModule) ---------------------------------------------
// Static per-pin capability for one GPIO, so the pin ownership map can flag a claim that lands on
// an unsafe pin: an output role driven onto an input-only pin or a boot strap, or any role on a
// reserved (flash/PSRAM/USB) pin. Domain-neutral; no chip API escapes src/platform/. ESP32 fills
// `validGpio`/`outputCapable`/`rtc` from the IDF's own GPIO_IS_VALID_GPIO / GPIO_IS_VALID_OUTPUT_
// GPIO / rtc_gpio_is_valid_gpio (the textbook, always-correct SDK queries), and overlays `strap` /
// `reserved` from a small per-chip table sourced from docs/reference/gpio-usage.md (the SDK has no
// "is this a strap / flash pin" query: that's board/datasheet knowledge). Desktop returns
// "all valid, nothing reserved" (a host build has no real GPIOs to protect). Pure lookup, no state.
struct GpioCapability {
    bool validGpio = true;      // a real, usable GPIO on this chip (false = out of range / not bonded)
    bool outputCapable = true;  // has an output driver (false = input-only, e.g. classic ESP32 34-39)
    bool rtc = false;           // an RTC/low-power-domain pin (usable for deep-sleep wake / RTC I/O)
    bool strap = false;         // a boot-strapping pin: driving it at reset can change boot mode
    bool reserved = false;      // wired to flash / PSRAM / native USB: routing I/O here corrupts the device
};
GpioCapability gpioCapability(uint8_t gpio);
// Test-only (desktop): make gpioCapability(gpio) return `cap` for one specific gpio, so PinsModule's
// severity derivation (reserved→error, driven-role-on-strap/input-only→warn) is testable on the host
// where every real pin is otherwise "safe". Call per gpio under test; reset in release.
void setTestGpioCapability(uint8_t gpio, GpioCapability cap);
void clearTestGpioCapability();

// Live electrical state of one GPIO: the pin map's second axis (what a pin is *doing now*, vs.
// gpioCapability's static "what it *is*"). The see-the-wire HAL check: gpio_get_level reads the pad on
// ANY pin, even one a peripheral drives, so a driver's output must toggle when it renders and a mic
// clock must toggle when the mic runs. Sampled on tick1s (off the hot path), not per frame. Desktop
// returns valid=false (no real pins) so the UI omits the live columns there.
struct GpioLiveState {
    bool valid = false;    // pin is readable (false = out of range, or desktop → columns omitted)
    bool level = false;    // current pad level: true = HIGH, false = LOW (gpio_get_level)
    bool output = false;   // the pad's output driver is enabled RIGHT NOW (gpio_get_io_config .oe)
    bool input = false;    // the pad's input buffer is enabled RIGHT NOW (.ie): a pin can be both
    uint8_t driveCap = 0;  // output drive strength 0..3 = WEAK / MEDIUM / STRONG / STRONGEST
};
GpioLiveState gpioLiveState(uint8_t gpio);
// Test-only (desktop): inject a live state for one gpio so PinsModule's level/drive columns are
// host-testable. Same shape as setTestGpioCapability.
void setTestGpioLiveState(uint8_t gpio, GpioLiveState state);
void clearTestGpioLiveState();

// Test-only cap on the value maxAllocBlock() reports; 0 = no cap (real value).
// Lets a test force MappingLUT's paged-destinations fallback without an actual
// fragmented heap. Production never calls this; reset to 0 in release.
void setTestMaxAllocBlock(size_t bytes);
                                // (scarce; use this as the memory-pressure KPI).
                                // PSRAM blocks dominate on S3/S2 boards and make
                                // maxAllocBlock useless as a stress signal -
                                // it'll report ~8 MB even when DRAM is exhausted.
size_t totalHeap();         // total heap capacity (internal + PSRAM)
size_t totalInternalHeap(); // total internal heap capacity

// Heap to keep free for stack, HTTP, WiFi, and overhead when sizing buffers -
// a platform memory constraint, not a domain one (it guards core subsystems).
// Any allocator checks free heap against this reserve before committing.
constexpr size_t HEAP_RESERVE = 32768;

void getMacAddress(uint8_t mac[6]);
// The MAC as canonical "XX:XX:XX:XX:XX:XX", formatted once into a static buffer: a stable per-chip
// identity string a caller can point at without keeping its own copy. (chipModel/sdkVersion likewise
// return static strings; a ReadOnly control binds straight to these, storing nothing per-module.)
const char* macString();
const char* chipModel();
const char* sdkVersion();

// CPU frequency + core count as one short static string ("240 MHz, 2 cores"), read from the RUNNING
// hardware, not a config macro: so a stale sdkconfig or a PM downclock is visible in the UI (finding
// the chip silently at 160 MHz is exactly what this control exists to catch). Desktop reports cores
// only (host clock speed has no portable query). Static-buffer contract as macString above.
const char* cpuInfo();

// PSRAM interface type as a short static string: "quad" (1-line SPI, classic ESP32 / WROVER) or
// "octal" (8-line, the S3/S2 -R8 parts). Derived from the compile-time CONFIG_SPIRAM_MODE (there is no
// runtime IDF query for the mode), so it reflects how the firmware drives the PSRAM. Empty "" when
// PSRAM is not enabled in this build. SystemModule shows it beside the psram usage so the type is
// visible (an S3 board reads "octal", a WROVER "quad"). Desktop returns "".
const char* psramType();

// WiFi co-processor status, for boards whose radio lives on a separate chip (the
// ESP32-P4 + on-board ESP32-C6 over esp_hosted). Returns a short status string:
// the detected co-processor firmware version when the query answers (e.g.
// "C6 fw 2.12.9"), "querying…" while attempts remain, "no version reply" once a
// bounded number of attempts have gone unanswered, or "" on targets with a native
// radio (no co-processor). Empty string => render nothing.
//
// "no version reply" rather than "not detected": on the bench the C6 associates and
// serves traffic while this particular RPC times out, so declaring the slave absent
// would be a false statement about working hardware. The field says what is known -
// the query did not answer: and leaves the conclusion to whoever reads it.
const char* coprocessorWifi();

// This host's LAN IPv4 address as a dotted string, or "" if unavailable.
// Desktop: the outbound interface address. ESP32: empty: the device IP is
// owned by NetworkModule (WiFi/Ethernet), not the platform layer.
const char* hostIp();

// Human-readable reset reason: "POWERON", "SW", "PANIC", "INT_WDT", "TASK_WDT",
// "BROWNOUT", "DEEPSLEEP", or "UNKNOWN". On desktop always returns "OK". UI uses
// this to flag a "crashed" prior boot (PANIC / INT_WDT / TASK_WDT / BROWNOUT).
const char* resetReason();

// Serial log verbosity, low to high. Mirrors the standard syslog/ESP-IDF ordering so the
// numeric value maps straight onto esp_log_level_set (None=0 … Verbose=5). The periodic KPI
// tick line (a plain stdout printf, not an ESP_LOG) is emitted only at Info or above, so a
// resting device at Warn stays quiet on the wire: no once-a-second serial write: while real
// ESP_LOGW/ESP_LOGE warnings and errors still print. setLogLevel applies it to the ESP-IDF
// logger; the KPI-line gate is read from the same value in the main loop. Desktop is a no-op.
enum class LogLevel : uint8_t { None = 0, Error, Warn, Info, Debug, Verbose };
void setLogLevel(LogLevel level);

size_t firmwareSize();        // firmware image bytes
size_t firmwarePartition();   // app partition size (firmware capacity)
size_t flashChipSize();       // total flash chip capacity
size_t filesystemUsed();      // filesystem used bytes
size_t filesystemTotal();     // filesystem total bytes

// Filesystem: LittleFS on ESP32, std::filesystem on desktop (rooted at ./.config/'s parent).
// Paths are absolute-looking (start with '/'); desktop strips the leading '/' so
// "/.config/System.json" maps to "<root>/.config/System.json".
//
// fsSetRoot redirects the desktop root to an absolute path. Used by unit tests to give each
// TEST_CASE an isolated working directory without chdir. No-op on ESP32 (LittleFS is mounted at a
// fixed partition). Must be called BEFORE fsMount. Passing null or "" restores the default, which
// on desktop is MM_DATA_DIR, else "build" inside a repo checkout, else the OS per-user data
// directory (see platform_desktop.cpp).
void fsSetRoot(const char* path);
const char* fsRootPath();                                    // the resolved root, for diagnostics
bool fsMount();                                              // idempotent; safe to call multiple times
void fsUnmount();
bool fsMkdir(const char* path);                              // mkdir -p; no error if exists
bool fsExists(const char* path);
bool fsRemove(const char* path);                             // file or empty dir
int  fsRead(const char* path, char* buf, size_t maxLen);     // bytes read; -1 on error; null-terminated on success
long fsSize(const char* path);                               // file size in bytes; -1 if missing/not a file
int  fsReadAt(const char* path, long offset, char* buf, size_t len);
                                                             // pread-style: read up to `len` bytes at `offset`; bytes read, 0 at EOF, -1 on error. NOT null-terminated. Lets a caller stream a file in fixed chunks.
bool fsWriteAtomic(const char* path, const char* data, size_t len);
                                                              // writes <path>.tmp, fsync, rename. Caller ensures parent dir exists.
// Streamed atomic write: open <path>.tmp, repeatedly call src(buf, cap, user, &abort) to pull up to
// `cap` bytes, fwrite each chunk, then fsync + rename into place. The source returns the byte count;
// 0 means *end*, BUT it distinguishes a clean end from a failed/incomplete one via `*abort`: a 0
// with `*abort == false` is a clean EOF (commit), a 0 (or any return) with `*abort == true` is an
// error (a short/timed-out upload) → the temp file is DISCARDED, not renamed. Returns false on abort,
// a write failure, or a rename failure. Lets the HTTP layer stream an upload of any size with a fixed
// small buffer: the device never holds the whole file in RAM. Caller ensures the parent dir exists.
using FsWriteSrc = size_t(*)(char* buf, size_t cap, void* user, bool* abort);
bool fsWriteStream(const char* path, FsWriteSrc src, void* user);
// Per-entry callback for fsList: name, whether it's a directory, and its size in bytes
// (0 for a directory). One level, one call per child.
using FsListCb = void(*)(const char* name, bool isDir, uint32_t sizeBytes, void* user);
void fsList(const char* dir, FsListCb cb, void* user);       // single-level listing

// Network (ESP32 only, stubs on desktop)
// setEthConfig overrides the per-chip default eth pin/PHY map (ethConfigDefault)
// with a board's runtime config before ethInit: NetworkModule pushes the values
// it got from deviceModels.json. Call before ethInit(); takes effect on the next init.
void setEthConfig(const EthPinConfig& cfg);
bool ethInit();
// Tear down a running Ethernet driver so ethInit() can re-init with new config -
// the live reconfigure path (used for W5500/SPI, which tears down cleanly; RMII
// keeps apply-on-next-init). Safe to call when nothing is running. Desktop: no-op.
void ethStop();
bool ethLinkUp() MM_NONBLOCKING;       // PHY link detected (cable plugged, fast check)
bool ethConnected() MM_NONBLOCKING;    // IP assigned (DHCP complete)
// Current IP as raw octets: out[0..3]. All-zero (0.0.0.0) means "no IP yet".
// Octets, not a string: the IP's canonical form is uint8_t[4] (matching the
// static-IP controls and formatDottedQuad); callers that need text format at
// their own boundary, callers that need bytes (ArtNet) use them directly.
void ethGetIPv4(uint8_t out[4]) MM_NONBLOCKING;

// Put one complete Ethernet frame on the wire, below IP: `frame` starts at the destination MAC and
// carries its own EtherType, so nothing here has an address, a route, or a DHCP lease. Panel
// receiver cards are addressed this way: they answer to a MAC and an EtherType,
// never to an IP.
//
// Needs a link, not an IP: ethLinkUp() is the precondition, ethConnected() is not. That split is
// the point of the seam: a board whose DHCP never completes can still drive panels, and the
// driver's status says which of the two is missing.
//
// `len` is the payload as handed to the MAC: below 60 bytes the hardware pads to the Ethernet
// minimum, so callers need not. Returns false when no driver is running, the link is down, or the
// MAC rejects the frame (a full TX ring): a dropped frame, like a dropped UDP packet, is the
// caller's to tolerate. Desktop records the frame instead of sending it, which is what lets the
// driver and its tests run on the host.
bool ethSendRaw(const uint8_t* frame, size_t len) MM_NONBLOCKING;

// End of one wall frame: hand whatever ethSendRaw batched to the wire, and start a new batch.
//
// Exists because a panel wall is a BURST, not a stream: a 128-row wall is ~131 frames that must all
// land inside the inter-frame window, since the cards have no buffering and latch on the sync frame.
// Where the platform can hand the whole burst to the kernel at once it should, and only the caller
// knows where a burst ends: hence a seam rather than a heuristic on frame contents, which would put
// wire-format knowledge in the platform layer.
//
// Idempotent and safe to call with nothing pending. A platform that already sends each frame as it
// arrives (ESP32's MAC, Linux, macOS) implements this as a no-op, so a driver calls it unconditionally.
void ethFlushRaw() MM_NONBLOCKING;

// Claim the Ethernet interface for direct L2 use, or release it. A driver that addresses the wire
// below IP calls this in prepare/release to STATE its intent, rather than leaving
// NetworkModule to infer it from traffic: the driver knows, and a claim made before the first frame
// cannot race the cascade's DHCP timeout.
//
// What it changes: nothing about the hardware. Ethernet keeps running and the cascade still moves on
// to WiFi for IP service (which is what a panel rig wants: panels on the wire, UI over WiFi). It
// only tells NetworkModule that a leaseless link is intended rather than broken.
//
// Reference-counted, so two drivers sharing the link both have to release before the claim drops.
void ethClaimRawL2(bool claim);

// True while any driver holds a raw-L2 claim. NetworkModule reads this to tell "Ethernet is broken"
// apart from "Ethernet carries no IP because something is driving it directly".
bool ethRawL2Claimed() MM_NONBLOCKING;

// Failures split by cause since boot, because the two are different faults with different fixes and
// a single counter cannot tell them apart:
//   linkDown: esp_eth_transmit refused before touching the MAC because the driver's link reads
//              down. A flapping PHY shows up here, and each flap costs seconds of refusals.
//   ringFull: the MAC had no free TX descriptor. That is back-pressure: our sender outrunning the
//              wire, or the DMA draining slower than it should.
void ethSendFailCounts(uint32_t& linkDown, uint32_t& ringFull) MM_NONBLOCKING;

// Consecutive ethSendRaw() failures since the last success. A raw sender polls this to tell a
// dropped frame (normal back-pressure, count returns to 0) from a wedged transmit path: the IDF
// driver refuses every frame once ITS link flag reads down, which can outlive our own event-driven
// flag and would otherwise look like a healthy link sending nothing.
uint32_t ethSendFailStreak() MM_NONBLOCKING;

// Restart the Ethernet driver after transmit has provably wedged: esp_eth_transmit refuses every
// frame once the driver's internal link state reads down, and that state can diverge from both the
// PHY and our own event-driven flag, observed on an S31 under sustained TX, with the link genuinely
// lost, no DISCONNECTED event delivered, and nothing recovering short of a reboot.
//
// stop/start re-runs the driver's link negotiation, which is the only supported way back (no ioctl
// writes the link flag). Heavier than a register poke, so a caller must gate it on a long failure
// streak rather than on ordinary back-pressure. Returns true when the restart succeeded; the link
// may still be down afterwards if the cable really is out, which is the honest outcome.
//
// BLOCKS FOR UP TO ~4 SECONDS, and the caller must treat that as the cost of the call. esp_eth_start
// restarts autonegotiation, which polls the PHY status register with vTaskDelay(100 ms) up to
// autonego_timeout_ms (4000 by default, and a wedged link is down so the loop runs to the timeout).
// NOT MM_NONBLOCKING for that reason.
//
// The trade is deliberate: the tick that calls this is the 1 Hz housekeeping tick, and it only fires
// in a wedged state where every frame is being refused anyway. A stalled render loop for one tick
// costs nothing a user can see when the wall is already dark, and the alternative is a wall that
// stays dark until someone power-cycles the board. Called at most once per wedge, never in a loop.
bool ethRestartTx();

// Negotiated link speed in Mbit/s (10 / 100 / 1000); 0 when no link or no driver. Reported rather
// than enforced: panel cards want a gigabit link, and a driver that knows the actual speed can say
// "100 Mbit, expect tearing" instead of either failing silently or refusing to run.
uint16_t ethLinkSpeedMbps() MM_NONBLOCKING;

// Bind raw sending to a host network interface by name ("eth0", "en0"). ESP32 ignores this: it has
// one MAC and ethSendRaw always uses it. On desktop it opens the raw socket (Linux AF_PACKET, macOS
// BPF) that makes a host a real panel controller: the same driver on a Pi or a mini-PC drives the
// same cards, which is worth having both as a product and as the way to test the wire format
// without an ESP32 in the loop.
//
// Returns false when the interface is unknown or the process lacks the privilege (raw L2 is
// root/CAP_NET_RAW on every OS). Failure is not fatal: sending falls back to CAPTURE mode, where
// frames are recorded for the tests and the driver reports why nothing reached the wire. Call with
// nullptr or "" to return to capture mode.
bool ethBindRawInterface(const char* ifName);

// Enumerate the host NICs raw sending could bind, for the panel-card driver's `interface`
// Select: display labels out (Windows: Npcap's friendly descriptions; POSIX: interface names),
// entry 0 always "none (capture only)". Rebuilt on every call so a hot-plugged NIC appears on
// the next schema rebuild. rawInterfaceName(i) is the BIND name behind row i (the pcap device
// name on Windows differs from its label; on POSIX they are the same); nullptr for row 0.
// A label may carry a live DETAIL after ", " -- Windows appends the adapter's link speed
// ("Realtek PCIe GbE Family Controller, 1 Gb") -- because the name alone does not tell a picker
// which entry is the 1 Gb NIC and which is a Wi-Fi radio or a Hyper-V virtual switch. Only the
// part BEFORE that separator is the adapter's identity: the speed changes when a link
// renegotiates, and both the apply path (Control.cpp) and the driver's own remap compare on the
// stable head so a changed speed does not read as a different NIC.
//
// The Select persists by LABEL (see Control::persistLabel): a NIC keeps its identity across
// reboots and Npcap reinstalls, the index-mismatch trap this exists to close.
size_t rawInterfaces(const char* const** optionsOut);
const char* rawInterfaceName(size_t i);
#ifndef ESP_PLATFORM
// Test seam: replace the enumeration with a fixed list (label = bind name), so the control's
// behavior is pinnable without real NICs. nullptr count 0 restores the real enumeration.
void setTestRawInterfaces(const char* const* names, size_t count);
#endif

// --- NDI video output -------------------------------------------------------------------------
//
// projectMM as an NDI source: the rendered frame reaches OBS, Resolume, TouchDesigner or any other
// NDI receiver, on this machine or another. Gated by `hasNdi` (desktop true, ESP32 false).
//
// **The runtime is the USER'S, never ours.** projectMM is GPL-3.0 and the NDI runtime is
// proprietary with redistribution terms GPL cannot carry downstream, so it is resolved on demand
// (dlopen / LoadLibrary) and never linked, never bundled, and its headers are never included: the
// same arrangement, and for the same reason, as Npcap for raw Ethernet. A machine without it builds
// and runs identically; ndiAvailable() simply reads false and the driver says so.

// Is the NDI runtime present and loaded? False when it is not installed, which is not an error -
// the driver reports it as a status. Loads on first call.
bool ndiAvailable();

// Create a named NDI source. `name` is what a receiver lists (a device name, typically). Returns
// false when the runtime is absent or creation fails. Replaces any sender already open.
bool ndiSenderOpen(const char* name);

// Destroy the sender. Safe with none open, so a driver's release() need not track state.
void ndiSenderClose();

// Send one frame: tightly-packed RGB, 3 bytes per pixel, w*h*3 bytes. `fps` is declared to the
// receiver as the frame rate. Returns false when no sender is open. The platform layer owns the
// conversion to NDI's own frame layout, so no NDI type reaches the light domain.
bool ndiSendFrame(const uint8_t* rgb, uint16_t w, uint16_t h, uint8_t fps);

// Desktop-only test seam, mirroring ethTestFrame* above: with no NDI runtime installed there is
// nothing to send into, and CI never has one, so the frames the driver produced are RECORDED
// instead. That is what lets the conversion be pinned: the geometry, the packing, the pacing -
// without the proprietary runtime, leaving the bench to confirm only that a receiver sees it.
#ifndef ESP_PLATFORM
// Force the runtime's apparent presence, overriding whatever is really installed. A developer
// machine may well HAVE a real NDI runtime (Resolume and NDI Tools both ship one), so a test that
// wants the not-installed path must be able to say so rather than rely on the machine lacking it.
// `true` also makes ndiSenderOpen()/ndiSendFrame() record instead of touching the real runtime.
enum class NdiTestMode : uint8_t { Off, ForceAvailable, ForceMissing };
void setTestNdiMode(NdiTestMode mode);
inline void setTestNdiAvailable(bool available) {
    setTestNdiMode(available ? NdiTestMode::ForceAvailable : NdiTestMode::Off);
}
size_t ndiTestFrameCount();
// The recorded frame's geometry and its tight-RGB bytes (w*h*3), as the driver handed them over.
uint16_t ndiTestFrameWidth(size_t i);
uint16_t ndiTestFrameHeight(size_t i);
uint8_t  ndiTestFrameFps(size_t i);
const uint8_t* ndiTestFrameData(size_t i);
// The name the sender was opened with, so a test can pin the blank-means-device-name rule.
const char* ndiTestSenderName();
void ndiTestClearFrames();
#endif

// --- HLS video output -------------------------------------------------------------------------
//
// projectMM as an HLS source: the rendered grid, output correction applied and any integer
// upscaling done (see HlsDriver's `scale`), reaches a TV, VLC or a browser as H.264 over HLS.
// Gated by `hasHls`.
//
// **The seam carries NUMBERS, not an encoder command line.** The driver states the frame geometry,
// the rate and the bitrate; how those become H.264 is entirely the platform's business, because
// the two implementations share no mechanism:
//
// - **Desktop** spawns the `ffmpeg` found on PATH and pipes raw frames to its stdin, ffmpeg doing
//   both the encode and the HLS segmenting. **ffmpeg is the USER'S, never ours**: nothing is
//   vendored or linked, the same runtime-dependency arrangement as Npcap and NDI. A machine
//   without ffmpeg builds and runs identically; encoderStart() fails and the driver says so.
// - **ESP32-P4** drives the chip's hardware H.264 encoder and muxes the segments itself, in RAM.
//
// An argv-shaped seam would have forced the P4 to string-parse `-s`/`-r`/`-b:v` back out of a
// command line assembled for a program it never runs.

/// What to encode. Geometry and rate are the frame contract; `encoderName` names a desktop ffmpeg
/// encoder and is ignored where the platform has only one (see `hasEncoderChoice`).
struct EncoderConfig {
    uint16_t    width;
    uint16_t    height;
    uint8_t     fps;           // also the GOP: hls_time can only cut on a keyframe, so a longer
                               // GOP silently lengthens every segment past the 1 s design
    uint16_t    bitrateKbit;
    const char* encoderName;   // e.g. "libx264"; nullptr or ignored where there is no choice
    const char* outDir;        // absolute directory for the playlist and segments (fs platforms)
};

// Start encoding to `cfg`. Replaces any encoder already running. Returns false when the platform
// cannot start one (desktop: ffmpeg absent or the spawn failed): not an error: the driver reports
// a status.
bool encoderStart(const EncoderConfig& cfg);

// Hand one whole frame to the encoder. The frame is COPIED into a bounded queue and a platform
// writer thread does the blocking pipe writes, so this never blocks the caller and a frame is
// always written whole (tearing is structurally impossible). Returns len when queued, 0 when
// the queue is full (the encoder is behind: the caller DROPS the frame and stays live), and -1
// when the writer saw the process die (EPIPE et al.; the caller restarts it).
int encoderWrite(const uint8_t* data, size_t len);

// Is the spawned encoder still alive? Reaps the child when it exited.
bool encoderRunning();

// Stop the encoder: close stdin (lets ffmpeg finalize the playlist), brief wait, then kill.
// Safe with none running, so a driver's release() need not track state.
void encoderStop();

// Serve an HLS file the platform holds in RAM rather than on the filesystem. Returns false when
// this platform writes segments to disk (desktop: ffmpeg does), and the caller falls through to
// its normal file path. The P4 keeps segments in a PSRAM ring because at one per second the
// flash wear buys nothing: a live segment is stale within seconds. `name` is a bare filename
// ("stream.m3u8", "seg7.ts"); the returned pointer stays valid until the next encoderWrite.
bool hlsSegment(const char* name, const uint8_t** data, size_t* len);
// Release the segment a preceding hlsSegment() handed out, once the caller has finished reading
// it. Required after every hlsSegment() that returned true: until it is called the platform will
// not recycle that segment's memory, so a missed release stalls the ring by one slot.
void hlsSegmentRelease();

#ifndef ESP_PLATFORM
// Test seam, mirroring NdiTestMode: CI has no ffmpeg and a test must not need one. Record mode
// makes encoderStart() a no-op recorder of its argv and encoderWrite() a frame recorder;
// ForceMissing makes encoderStart() fail, for the not-installed path.
enum class EncoderTestMode : uint8_t { Off, Record, ForceMissing };
void setTestEncoderMode(EncoderTestMode mode);
// Force encoderWrite's next results in Record mode (0 = would-block, -1 = dead), so the driver's
// drop counter and restart path are pinnable; any other value records normally again.
void setTestEncoderWriteResult(int result);
size_t encoderTestFrameCount();
size_t encoderTestFrameSize(size_t i);
const uint8_t* encoderTestFrameData(size_t i);
// The argv encoderStart was called with, joined by single spaces (for pinning the arg builder).
const char* encoderTestArgs();
void encoderTestClearFrames();
#endif

// Desktop-only test seam: the frames ethSendRaw() captured, so a host test can pin what the driver
// put on the wire. Active whenever no raw interface is bound, which is the default and the only
// mode an unprivileged test process ever sees. Count resets with ethTestClearFrames(); a frame
// longer than kEthTestFrameMax is recorded truncated (its true length still reported) so an
// oversized send is visible, not silent.
#ifndef ESP_PLATFORM
constexpr size_t kEthTestFrameMax = 1512;   // the panel format's largest frame
size_t ethTestFrameCount();
size_t ethTestFrameLength(size_t i);
const uint8_t* ethTestFrameData(size_t i);
void ethTestClearFrames();
// Make the next ethSendRaw() calls fail, so a test can exercise the link-down path
// (mirrors setTestBindFails for UdpSocket).
void setTestEthSendFails(bool fail);
// Override the reported link speed so a test can exercise the too-slow-link status.
void setTestEthLinkSpeed(uint16_t mbps);
// Make ethRestartTx() fail, so a test can exercise the recovery-failed path, the one case
// that strands transmit and must stay visible rather than reading as an unplugged cable.
void setTestEthRestartFails(bool fail);
// How many times ethRestartTx() has run, so a test can pin the once-per-wedge bound (the driver's
// own restartTried_ is private, and the count is what the bound is actually about).
uint32_t ethRestartCountForTest();
#endif

bool wifiStaInit(const char* ssid, const char* password);
bool wifiStaConnected() MM_NONBLOCKING;
void wifiStaGetIPv4(uint8_t out[4]);   // see ethGetIPv4: same octet contract
void wifiStaStop();

// STA-side RSSI in dBm (negative, e.g. -58). Returns 0 when the STA isn't
// associated or the call fails: NetworkModule only surfaces this control
// while state_ == ConnectedSta so a 0 is effectively unreachable.
int wifiStaRssi();

// STA-side AP info for the WLED /json `wifi` block: the associated AP's BSSID
// (6 octets into `out`) and the WiFi channel. Both zeroed (all-zero BSSID /
// channel 0) when the STA isn't associated or the call fails.
void wifiStaBssid(uint8_t out[6]);
int  wifiStaChannel();

// A client interface (STA or Ethernet) whose addressing NetworkModule sets. One enum so a
// single netSetStaticIPv4 serves both, rather than a per-interface duplicate. (The AP is not
// here: it is always the DHCP *server* at a fixed IP, a different role: wifiApInit sets that.)
enum class NetIface : uint8_t { Sta, Eth };
// Switch a client interface to a STATIC IPv4 config: stop its DHCP client and pin ip/gateway/mask
// (+ DNS if non-zero) onto the netif. Octets, matching ethGetIPv4/wifiStaGetIPv4. Passing an
// all-zero `ip` is a no-op guard (treated as "not static"). Idempotent: safe to re-apply. To go
// back to DHCP, call netSetDhcp(iface). Desktop: no-op (host uses OS networking). The netif must
// exist (interface init has run); NetworkModule calls this after bring-up / on a live toggle.
void netSetStaticIPv4(NetIface iface, const uint8_t ip[4], const uint8_t gw[4],
                      const uint8_t mask[4], const uint8_t dns[4]);
// Return a client interface to DHCP: restart its DHCP client so it re-leases live (no reboot).
// The counterpart to netSetStaticIPv4 for a Static→DHCP toggle. Desktop: no-op.
void netSetDhcp(NetIface iface);
// Test seams (desktop-only impls, same contract as setTestBindFails: reset in release so cases
// stay independent): make wifiStaInit() succeed so a host test can drive the STA cascade
// (WaitingSta) that the radio-less desktop otherwise never enters, and count netSetStaticIPv4()
// applies per interface so the test can pin that the static-addressing path reached the platform.
void setTestWifiStaAvailable(bool available);
uint32_t testNetStaticApplyCount(NetIface iface);

bool wifiApInit(const char* apName, const char* ip);
bool wifiApConnected();
void wifiApStop();
// Stations currently associated with our SoftAP. 0 when the AP is down or nobody is on it.
// NetworkModule's AP fallback uses this to hold off its periodic STA retry while somebody is using
// the portal: bringing STA up switches the radio to STA mode, which drops the AP under them.
uint32_t wifiApClientCount();

// True when it is safe to open/use a socket: the TCP/IP stack is initialized and
// an interface has an IP. On ESP32 that means Ethernet or WiFi (STA/AP) is up -
// calling any lwip socket API before then asserts (the core mutex is still null).
// Desktop: always true (host sockets work regardless of link state). Callers that
// open sockets at boot (before NetworkModule brings an interface up) must gate on
// this and open lazily from the tick path once it turns true.
bool networkReady();

// Current WiFi transmit power, in dBm (ESP-IDF reports quarter-dBm internally
// and we round to whole). Returns 0 when WiFi isn't initialized or the call
// fails. Same value for STA and AP: WiFi has one radio at one TX power.
int wifiTxPower();

// Cap the WiFi transmit power. `quarterDbm` is in ESP-IDF's quarter-dBm units
// (valid range 8..84 → 2..21 dBm); pass 0 to skip the override and let the
// stack use its default. Used by NetworkModule for the weak-power / brown-out
// WiFi cap: some boards / WiFi modules (a thin on-module LDO, a marginal USB
// supply: e.g. various S2/S3 mini-class boards) brown out at full TX power,
// dropping WiFi during association. Capping to 8 dBm (32 quarter-dBm, the value
// `deviceModels.json` injects for brown-out-prone entries) keeps them stable. Returns
// true on success or when called with 0 (no-op).
// Call after esp_wifi_start(): earlier calls are silently ignored by ESP-IDF.
bool wifiSetTxPower(int8_t quarterDbm);

// mDNS is advertise-only: `mdnsInit` brings the stack up and advertises this device as
// `_http._tcp` (with an `mm=1` TXT) and `_wled._tcp` (with a `mac=` TXT), so the native
// WLED app + Home Assistant discover it. Peer discovery is UDP presence (DevicesModule +
// WledPacket): the platform exposes advertise here, discovery lives in the module.
bool mdnsInit(const char* deviceName);
// Stop advertising: remove both services and clear the hostname, keeping the stack up so
// a later mdnsInit re-advertises without a full re-init (the mDNS toggle uses this).
void mdnsStop();
// Full mdns_free: call at release.
void mdnsShutdown();

// Store the DHCP hostname (DHCP option 12) the next eth/wifi bring-up advertises.
// Routers populate their client list from the DHCP request, not mDNS, so without
// this a provisioned device shows as "Unknown" there. Call before ethInit() /
// wifiStaInit(): the netif applies it before the DHCP client starts, so it lands
// in the first DISCOVER. NetworkModule pushes deviceName (default MM-XXXX), keeping
// the router name, the mDNS .local name and the SoftAP name one identity. A later
// rename takes effect on the next DHCP renewal/reconnect. Desktop: no-op.
void setHostname(const char* name);

// OTA: fetch a firmware image from `url` and flash it to the next OTA partition.
// ESP32: spawns a one-shot FreeRTOS task (the call returns immediately; the task
// runs to completion or error). The task uses `esp_https_ota`, which rolls the
// download + partition write + boot-pointer flip into one API; on success it
// calls platform::reboot() so the device boots the new image.
// Desktop: returns false (no OTA partition to write to); call sites guard with
// `if constexpr (mm::platform::hasOta)`.
//
// `statusBuf` is updated in place by the task with a short progress string
// (e.g. "downloading", "flashing", "error: HTTP 404"). `bytesReadOut` /
// `bytesTotalOut` advance as the download proceeds: the UI renders them as
// "X KB / Y KB". `bytesTotalOut` is 0 until esp_https_ota reports the image
// size (just after the HTTPS handshake), then holds the real value for the
// rest of the task's lifetime. FirmwareUpdateModule polls all three at 1 Hz
// and copies into its Control buffers so the WS state push surfaces progress
// to the UI without extra wiring.
bool http_fetch_to_ota(const char* url,
                       char* statusBuf, size_t statusBufLen,
                       uint32_t* bytesReadOut, uint32_t* bytesTotalOut);

// OTA: flash a firmware image STREAMED from `src` (no URL fetch; the caller pulls the bytes,
// e.g. straight off an HTTP upload body). Same producer callback shape as fsWriteStream: `src`
// fills up to `cap` bytes, returns the count (0 = clean EOF), and sets `*abort` to fail the OTA
// (an incomplete/timed-out upload). Runs esp_ota_begin → esp_ota_write per chunk → esp_ota_end +
// set_boot_partition, then RETURNS true (it does NOT reboot: the caller sends its HTTP 200 first,
// then reboots into the flashed image, the same order /api/reboot uses). SYNCHRONOUS (unlike
// http_fetch_to_ota, which runs on its own task): the caller is the HTTP request handler, which runs
// on the tick20ms tick INSIDE Scheduler::tick: so this blocks rendering for the flash duration. That
// is the accepted trade-off (a firmware upload is user-initiated and reboots the device on success),
// bounded by the same upload idle/hard limits; the caller needs the result to reply.
// `statusBuf` / `bytesReadOut` are updated in place (bytesTotal is the caller-supplied
// Content-Length, so no separate out-param). Desktop: returns false (no OTA partition); guard with
// `if constexpr (mm::platform::hasOta)`. Returns true iff the image flashed + boot pointer flipped.
bool otaWriteStream(FsWriteSrc src, void* user, size_t contentLen,
                    char* statusBuf, size_t statusBufLen, uint32_t* bytesReadOut);

// MOONBASE, the second boot image, present only on tables that carry a factory app (forced on a
// 4 MB board, chosen on the larger ones): a small, rarely changing firmware that owns the device
// when the application is not running or cannot be trusted. Its first job is installing firmware into the one large app slot,
// since a board cannot rewrite the partition it is executing from, so an update there is two
// stages: otaBootMoonBase() + reboot, then install from MoonBase.
// All of these are false / no-ops on a dual-OTA table and on desktop.
bool otaHasMoonBase();      // does this table carry MoonBase?
bool otaBootMoonBase();     // point the bootloader at it; false when there is none
bool otaRunningMoonBase();  // are we executing from it right now?
// Which MoonBase is installed, read from the factory partition's app descriptor without booting
// it. False when there is no MoonBase or the image carries no readable descriptor. The string is
// comparable with mm::kVersion: both come from the same computed version.
bool otaMoonBaseVersion(char* out, size_t len);
// The rest of the same descriptor, so the UI can show MoonBase's identity the way it shows the
// app's: when it was built, and how much of its slot it fills.
bool otaMoonBaseBuild(char* out, size_t len);
bool otaMoonBaseSize(uint32_t* used, uint32_t* total);
// Install a new MOONBASE, the inverse of MoonBase installing the app: the factory partition is
// writable only while the app is running, exactly as the app slot is only while MoonBase runs.
// Without this a device whose recovery image is broken needs a cable.
//
// Same producer-callback shape as otaWriteStream, and the same status vocabulary, with two
// differences that matter. It VETS the first chunk (magic, chip id, and that the image really is
// MoonBase) before erasing anything, so a wrong URL costs nothing. And it does NOT reboot: the
// running app is untouched, and the new image is simply what the device falls back to next.
bool otaWriteMoonBase(FsWriteSrc src, void* user, size_t contentLen,
                      char* statusBuf, size_t statusBufLen, uint32_t* bytesReadOut);
// The same install from a URL rather than an upload: the device fetches the image itself, which
// is what lets it take a release asset straight from GitHub. Runs on its OWN TASK and returns at
// once, like the app's URL install: while the request is open the browser cannot poll, so a
// synchronous install could only ever report "installing" and then "installed", with no progress
// in between. Nothing reboots, so the caller answers 202 and the UI follows the status.
bool otaFetchMoonBaseUrl(const char* url, char* statusBuf, size_t statusBufLen,
                         uint32_t* bytesReadOut, uint32_t* bytesTotalOut);
// Stage an install URL for MoonBase to pick up on its next boot (NVS namespace "moonbase",
// key "url", at most 255 bytes: MoonBase reads it into a 256-byte buffer and the HTTP route
// rejects anything longer). This is what makes a URL install unattended: the app stages the URL, reboots into
// MoonBase, and MoonBase installs it with no browser in the loop. MoonBase erases the key before
// attempting the install, so a bad URL cannot boot-loop the device.
bool moonbaseStageInstallUrl(const char* url);
// Erase a staged URL that never got consumed: a power cut between staging and the boot-partition
// switch leaves it armed, and the next unrelated MoonBase visit would auto-install it.
void moonbaseClearStagedUrl();

// Synchronous outbound HTTP request to a LAN host: plain HTTP, no TLS (the Philips Hue v1
// API, which HueDriver drives, allows it). Connects to `host:port`, sends `method path`
// with `reqBody` (NUL-terminated; "" for none: a Content-Length + JSON content-type are
// added when non-empty), and copies the RESPONSE BODY into `body` (NUL-terminated, truncated
// to bodyLen-1). Returns the HTTP status code, or 0 on connect/timeout/error. Blocks up to
// `timeoutMs`. Caller runs this OFF the render hot path (HueDriver on tick1s, like the OTA
// fetch / the old mDNS browse). Built on raw sockets, same primitives as the HTTP server.
int httpRequest(const char* method, const char* host, uint16_t port, const char* path,
                const char* reqBody, uint32_t timeoutMs, char* body, size_t bodyLen);

// Improv WiFi provisioning over UART0.
// ESP32 only; desktop stub returns false. Spawns a FreeRTOS task that installs
// a UART driver on UART_NUM_0 (the same channel ESP-IDF logging writes to;
// they coexist because logging uses direct register writes, not the driver).
// The task feeds inbound bytes into the `improv/improv` parser.
//
// On a provision request the task validates state: if wifiStaConnected() is
// true it emits Improv's wrong-state error frame. Otherwise it copies the
// credentials into the caller-owned buffers `ssidOut` / `passwordOut` (sized
// to hold 33 + 64 bytes, matching NetworkModule's storage) and sets `*ready`
// to true. The caller's tick1s() polls `ready`, copies the buffers onward,
// and clears the flag.
//
// `statusBuf` mirrors http_fetch_to_ota's pattern: the task writes short
// strings ("listening", "received credentials", "connecting",
// "connected: <ssid>", "error: <reason>"). ImprovProvisioningModule polls
// it into a read-only Control.
//
// `info` is borrowed; the task copies the strings on init (pass static
// storage like `kVersion` and SystemModule::deviceName()).
struct ImprovDeviceInfo {
    const char* name;            // device hostname, e.g. "MM-3A7F"
    const char* chipFamily;      // "ESP32" / "ESP32-S3" / ...
    const char* firmwareVersion; // e.g. "1.0.0-rc2"
};
// SET_TX_POWER RPC (command 0xFD): when set, the Improv task validates the
// 1-byte dBm payload (0..21), writes it to txPowerOut, and publishes via
// txPowerReady's release-store. This is the pre-association escape hatch for
// boards whose LDO browns out at full TX power (weak-powered boards): their
// catalog cap normally arrives over HTTP *after* the device is online,
// which such a board can never reach: proven on the bench 2026-06-10. It stays
// a dedicated RPC (not an APPLY_OP) precisely because it must land BEFORE the
// radio associates, whereas APPLY_OP ops apply once the device is up.
// opOut/opOutLen/opReady carry the APPLY_OP vendor RPC (0xFC): one REST operation
// as JSON, pushed over serial during provisioning ("Improv = REST over serial").
// Chunks reassemble into opOut; on the last chunk opReady's release-store publishes
// it and ImprovProvisioningModule applies the op on the main loop. This is how the
// deviceModel and every other catalog default arrive: a `set`/`add` op routed through
// the apply-core + per-control validators, the same path the HTTP API uses.
// Opt out by leaving null (desktop stub does).
bool improvProvisioningInit(const ImprovDeviceInfo& info,
                            char* ssidOut, size_t ssidOutLen,
                            char* passwordOut, size_t passwordOutLen,
                            std::atomic<bool>* ready,
                            char* statusBuf, size_t statusBufLen,
                            uint8_t* txPowerOut = nullptr,
                            std::atomic<bool>* txPowerReady = nullptr,
                            char* opOut = nullptr, size_t opOutLen = 0,
                            std::atomic<bool>* opReady = nullptr);

class UdpSocket {
public:
    UdpSocket() = default;
    ~UdpSocket();

    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    bool open();
    // Bind a fixed destination so each sendTo() skips the per-packet address
    // parse + route lookup. Returns false on a bad IP.
    bool connect(const char* ip, uint16_t port);
    bool sendTo(const uint8_t* data, size_t len);  // uses the connect()ed destination
    // Receiver side (ArtNet in): listen on `port` on any interface
    // (SO_REUSEADDR) and flip the socket non-blocking: note that flips the
    // whole socket, sendTo() included. Returns false when the port is taken.
    bool bind(uint16_t port);
    // Non-blocking receive of one datagram: >0 = bytes copied into buf, -1 =
    // nothing pending. Mirrors TcpConnection::read's contract minus the
    // peer-closed 0 case (UDP has no connection to close). A datagram longer
    // than maxLen is truncated. Pass `srcIp` to also get the sender's IPv4
    // octets (ArtNet discovery replies go back to the poller's address).
    int recvFrom(uint8_t* buf, size_t maxLen, uint8_t srcIp[4] = nullptr);
    // One-shot send to an explicit address: for replying on a bound,
    // unconnected receive socket (e.g. ArtPollReply to the poller). connect()ed
    // send sockets keep using sendTo().
    bool sendToAddr(const uint8_t ip[4], uint16_t port, const uint8_t* data, size_t len);
    // Join an IPv4 multicast group on a bound socket, so datagrams sent to that group are
    // delivered here. Required for WLED audio sync, which multicasts to 239.0.0.1 rather than
    // broadcasting: without the membership the OS never hands those datagrams to the socket,
    // however correct the port and the packet are. Call AFTER bind(). False = the join failed
    // (no route yet, no interface); the caller retries rather than treating it as fatal.
    bool joinMulticast(const char* group);
    void close();

private:
    int fd_ = -1;
};

class TcpConnection {
public:
    TcpConnection() = default;
    explicit TcpConnection(int fd) : fd_(fd) {}
    ~TcpConnection();

    TcpConnection(const TcpConnection&) = delete;
    TcpConnection& operator=(const TcpConnection&) = delete;

    TcpConnection(TcpConnection&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
    TcpConnection& operator=(TcpConnection&& other) noexcept {
        if (this != &other) { close(); fd_ = other.fd_; other.fd_ = -1; }
        return *this;
    }

    // Non-blocking outbound connect to host:port, for a client that must NOT stall the render loop
    // (MQTT runs on tick1s inside Scheduler::tick). `connectStart` resolves `host` (a hostname via
    // getaddrinfo: one bounded DNS lookup: or a dotted-quad IP) and kicks off a non-blocking
    // connect, returning immediately; `connectPoll` checks the in-flight connect WITHOUT blocking and
    // returns Pending / Connected / Failed. The caller polls across ticks and enforces its own overall
    // timeout, then reads/writes via the non-blocking read()/writeSome(). Caller gates on
    // networkReady(). Desktop + ESP32 (lwip); a fresh TcpConnection (no fd yet) is the receiver.
    enum class ConnectResult : uint8_t { Pending, Connected, Failed };
    bool connectStart(const char* host, uint16_t port);   // false = immediate failure (DNS/socket)
    ConnectResult connectPoll();                           // non-blocking; call after connectStart

    bool valid() const { return fd_ >= 0; }
    int read(uint8_t* buf, size_t maxLen);   // non-blocking: >0 data, 0 closed, -1 nothing
    bool write(const uint8_t* data, size_t len);  // blocking: sends all bytes (HTTP responses must complete)
    // Non-blocking partial write: send as many of `len` bytes as the socket accepts right
    // now, return the count actually written (0..len). -1 = socket error (caller closes);
    // 0 = WouldBlock (buffer full, try later) or len==0. The caller advances its own offset
    // and re-calls: used by the preview drain to stream a frame across ticks without ever
    // blocking the render task. Never spins, never yields. (int mirrors read()'s contract.)
    int writeSome(const uint8_t* data, size_t len);

    void close();

private:
    int fd_ = -1;
};

class TcpServer {
public:
    TcpServer() = default;
    ~TcpServer();

    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;

    bool open(uint16_t port);
    TcpConnection accept();  // non-blocking, returns invalid if none pending
    void close();

private:
    int fd_ = -1;
};

// Restart the device. On ESP32: hardware reset (esp_restart). On desktop: process exit.
// Does not return.
[[noreturn]] void reboot();

// ---------------------------------------------------------------------------
// RMT WS2812 LED output (classic ESP32 + S3 + P4). The driver (src/light/drivers/
// RmtLedDriver.h) does the symbol encode in domain code and may run several
// channels at once (one per pin); the platform owns only the peripheral. All
// no-ops on targets without RMT, so the driver compiles everywhere behind
// `if constexpr (platform::rmtTxChannels > 0)` (see platform_config.h) and is
// simply inert off the chips that have RMT.
// ---------------------------------------------------------------------------

// Opaque handle to one configured RMT TX channel. `impl` is set by the platform
// (a heap struct holding the channel + encoder); the driver never inspects it.
struct RmtWs2812Handle { void* impl = nullptr; };

// Allocate + configure one RMT TX channel on `gpio`. `resolutionHz` is the tick
// clock the caller expresses symbol durations in; `invert` flips output polarity
// for inverting level-shifters. Returns false on failure (and on non-ESP32).
bool rmtWs2812Init(RmtWs2812Handle& h, uint8_t gpio, uint32_t resolutionHz, bool invert);

// The tick resolution the platform actually granted (may differ from requested).
// The driver converts its ns timings to ticks with this. 0 if not initialized.
uint32_t rmtWs2812Resolution(const RmtWs2812Handle& h) MM_NONBLOCKING;

// Start transmitting `symbolCount` pre-encoded WS2812 RMT symbols and return
// immediately: channels started back-to-back clock out concurrently. Pair with
// rmtWs2812Wait; the caller owns the inter-frame latch (delayUs) after the last
// wait. The symbol buffer must stay valid until the wait returns. Returns false
// when the channel isn't initialized (and on targets without RMT).
bool rmtWs2812Transmit(RmtWs2812Handle& h, const uint32_t* symbols, size_t symbolCount);

// Block until the channel's in-flight transmission finishes, bounded by
// `timeoutMs` so a wedged peripheral can't hang the render tick forever: a
// timed-out frame is simply dropped and re-encoded next tick (self-heals). With
// N channels waited sequentially the worst case is N×timeoutMs. A timeout is NOT a dropped frame
// the caller may re-encode over: the peripheral is still reading the symbol buffer, so the driver
// keeps it untouched and waits again on the next tick (RmtLedDriver::waitForPins).
/// Block until this channel's transmit completes. Returns false on TIMEOUT, meaning the frame is
/// STILL CLOCKING OUT: the caller must not touch the symbol buffer it handed over, because the
/// peripheral is still reading it.
bool rmtWs2812Wait(RmtWs2812Handle& h, uint32_t timeoutMs);

void rmtWs2812Deinit(RmtWs2812Handle& h);

// RX loopback capture, on-device test only (no-op stub off ESP32). Capture up to
// `maxSymbols` pulse-duration symbols on `gpio` (jumpered from the TX pin) within
// `timeoutMs`. Returns the number captured. Used only by the loopback self-test.
size_t rmtWs2812RxCapture(uint8_t gpio, uint32_t resolutionHz,
                          uint32_t* outSymbols, size_t maxSymbols, uint32_t timeoutMs);


// Self-contained RMT loopback self-test, runnable from the running firmware (the
// RmtLedDriver's loopbackTest control). Drives a known WS2812 pattern out `txGpio`
// and captures it back on `rxGpio` (the user jumpers them), proving the GPIO emits
// correct bytes on real silicon. All hardware (RMT TX/RX, the GPIO continuity
// pre-check) lives here so src/light/ stays platform-free. No-op returning a
// "not supported" result off ESP32.
struct RmtLoopbackResult {
    bool jumperDetected = false;  // plain-GPIO continuity pre-check (tx high→rx high, low→low)
    bool pass = false;            // captured bytes == sent bytes (or whole frame bit-exact)
    uint8_t sent[3] = {};         // the per-light test pattern transmitted
    uint8_t got[3] = {};          // the light holding the first mismatch (light 0 when clean)
    uint32_t bitsChecked = 0;     // total WS2812 bits verified (frame mode); 24 for the short test
    uint32_t firstBadBit = 0;     // index of the first wrong bit, or bitsChecked when all pass
    // Capture diagnostics (frame mode). The verdict must say WHY it failed, not only that it did:
    // an empty capture (dead wiring, idle line, stalled transfer) is a different fault class from a
    // full capture that decodes wrong (waveform/threshold): without these numbers both collapse
    // into the same "bad bit 0/0" and the instrument can't isolate anything.
    uint32_t capturedSymbols = 0; // RMT RX symbols actually captured (target: >= bitsChecked)
    int8_t rxIdleLevel = -1;      // RX GPIO level sampled after the capture window (-1 = unknown)
    uint32_t txWallUs = 0;        // wall time of the first (timed) transmit
    uint32_t txExpectUs = 0;      // expected wire time (byte count / configured clock): a wall time
                                  // far above this is a stalled/underrun transfer, measured directly
};
RmtLoopbackResult rmtWs2812Loopback(uint8_t txGpio, uint8_t rxGpio);

// Whole-FRAME RMT loopback: instead of a 24-bit synthetic burst, transmit a
// real `lights`-light WS2812 frame (the per-light pattern 0xA5/0x00/0xFF
// repeated, `channels` per light) back to back like the render loop, capture
// the WHOLE frame on rxGpio and bit-verify every WS2812 bit. This is what
// catches frame-rate / sustained-transfer corruption and RF interference on
// the data line that a 24-bit burst can't: a single flipped bit anywhere in
// the frame fails the test and reports its position. No-op off ESP32.
RmtLoopbackResult rmtWs2812LoopbackFrame(uint8_t txGpio, uint8_t rxGpio,
                                         uint16_t lights, uint8_t channels);

// ---------------------------------------------------------------------------
// i80-bus parallel WS2812 output: the LCD_CAM peripheral on the ESP32-S3/P4, the
// I2S peripheral on the classic ESP32 (IDF's esp_lcd i80 API picks the backend per
// chip). The driver (src/light/drivers/MultiPinLedDriver.h) pre-encodes the WHOLE frame into one
// DMA buffer (3-slot encode in ParallelSlots.h, domain code); the platform owns
// only the i80 bus/peripheral AND the DMA buffer itself: the buffer must be
// DMA-capable internal RAM (platform::alloc prefers PSRAM, which the
// peripheral can't stream from at full rate), so the platform allocates it at
// init and exposes the pointer for the driver's zero-copy encode. All inert
// on targets without the i80 LCD peripheral, guarded by
// `if constexpr (platform::lcdLanes == 0)` in the driver.
// ---------------------------------------------------------------------------

// Opaque handle to one configured i80 bus + IO device + one or TWO DMA frame buffers.
struct I80Ws2812Handle { void* impl = nullptr; };

// Create the 8-lane bus on `dataPins[0..laneCount)` plus the two peripheral-
// mandated lines WS2812 strands ignore: `wrGpio` (the pixel clock) and
// `dcGpio` (data/command). Allocates buffer 0 (a zeroed DMA-capable frame of
// `bufferBytes`). When `wantSecondBuffer` is true (the async double-buffer is
// on), it also TRIES a second identical buffer and, if it fits, arms
// double-buffer mode; if it won't fit (memory-tight board), buffer 1 stays null
// and the driver runs single-buffer (allocate-and-degrade: the double-buffer
// is never *required*). When `wantSecondBuffer` is false (default), NO second
// buffer is allocated at all: the off path costs exactly one buffer. Returns
// false only when buffer 0 (or the bus) can't be created (bad pins, DMA pressure).
// `clockMultiplier` (1 = direct, 8 = a 74HCT595 shift-register expander on every data pin) scales
// the pixel clock: a '595 is serial-in, so each WS2812 slot is shifted out over that many bus
// words, and the bus must clock proportionally faster to keep the slot's duration on the wire.
// The backend picks the exact rate its clock tree can divide to EXACTLY (an inexact rate is not an
// error in esp_lcd: it silently rounds the prescale, which would emit a wrong waveform). A
// multiplier > 1 is rejected on a backend that cannot DMA the resulting frame from PSRAM (the
// classic-ESP32 I2S i80 path), rather than driving a frame the hardware can't sustain.
//
// `kBusPinUnset` for `wrGpio` / `dcGpio` (or a parked data lane) means "no pin": on the classic
// ESP32 the backend sinks that line onto an input-only pad, so the peripheral gets the GPIO number
// it insists on and nothing on the board is driven. The LCD_CAM backends need a real pad for both
// (the P4 ROM writes outside the GPIO block for an invalid number), so there it is an init failure
// the driver reports before ever calling this.
constexpr uint16_t kBusPinUnset = 0xFFFF;
bool i80Ws2812Init(I80Ws2812Handle& h, const uint16_t* dataPins, uint8_t laneCount,
                   uint16_t wrGpio, uint16_t dcGpio, size_t bufferBytes,
                   bool wantSecondBuffer, uint8_t clockMultiplier = 1);
// Why the last i80Ws2812Init returned false, when the backend knows more than "it failed"
// (a peripheral another module holds, a pin this package lacks); nullptr when it does not.
// The driver shows it as the status, so the user reads the cause rather than "check pins / memory".
const char* i80Ws2812LastError();
// Whether the peripheral this backend shares with other modules is free to claim right now. On the
// classic ESP32 the i80 bus is the I2S peripheral and a PDM microphone wants the same instance, so
// the driver polls this to rebuild itself once the microphone lets go (and the microphone's own
// retry does the mirror). Always false where nothing is shared. Cheap: a registry read, no init.
bool i80Ws2812SharedBusFree();

// DMA frame buffer `buffer` (0 or 1) the driver encodes into (zero-copy).
// Buffer 0 always exists once init succeeded; buffer 1 is null when the second
// allocation didn't fit: the driver reads that null as "run single-buffer".
// `i80Ws2812BufferCapacity` is the shared per-buffer capacity (both buffers are
// the same size): the driver's grow-only check. nullptr / 0 when not initialized.
uint8_t* i80Ws2812Buffer(const I80Ws2812Handle& h, uint8_t buffer);
size_t i80Ws2812BufferCapacity(const I80Ws2812Handle& h);

// Start the autonomous DMA transfer of buffer `buffer`'s first `bytes` and
// return; pair with i80Ws2812Wait on the SAME buffer. Once started no CPU work
// remains: there is no refill deadline for WiFi to miss (the design difference
// vs the ISR-refilled rings in the hpwit/FastLED lineage). The deferred-wait
// tick encodes into the other buffer while this one clocks out.
bool i80Ws2812Transmit(I80Ws2812Handle& h, uint8_t buffer, size_t bytes);

// Block until buffer `buffer`'s in-flight transfer finishes, bounded by `timeoutMs`.
// Returns TRUE only when the transfer actually completed. FALSE on timeout: the DMA may still be
// reading that buffer, so the caller must NOT re-encode into it (see ParallelLedDriver::busWaitIfBusy,
// which keeps it marked in-flight and re-waits next tick rather than corrupting a live transfer).
bool i80Ws2812Wait(I80Ws2812Handle& h, uint8_t buffer, uint32_t timeoutMs);

// Duration in microseconds of the most recent completed DMA transfer: measured start-of-transmit
// to done-callback, so it is the PURE wire/DMA time (independent of CPU / render load), i.e. the
// hard WS2812 output floor (256 lights × 30 µs ≈ 7680 µs → the 130 fps ceiling). The driver surfaces
// it as a read-only KPI so the actual output rate is visible as the pipeline improves (and if a
// future build overclocks the slot rate, this reflects it directly). 0 until the first transfer completes.
uint32_t i80Ws2812LastTransmitUs(const I80Ws2812Handle& h);

void i80Ws2812Deinit(I80Ws2812Handle& h);

// LCD loopback self-test: build a private FULL-WIDTH bus on the driver's
// real pins (the i80 peripheral configures all 8 data lines: a partial bus
// is rejected by the hardware layer) and transmit the caller's REAL encoded
// frame (`frame`/`frameBytes`, lane 0 = dataPins[0]) back to back, exactly
// like the render loop, while an RMT RX channel captures the whole frame off
// `rxGpio` and verifies every bit (RMT receive is transmitter-agnostic: the
// increment-1 rig reused). `dataBytes` is the slot-carrying prefix of the
// frame (before the latch pad); `rowBits` the bits per light row, so the
// expected pattern repeats per row. Testing the genuine frame matters: a
// short synthetic burst misses exactly the real-transfer failures (DMA
// descriptor boundaries, sustained-rate stalls). Same result shape as the
// RMT test; got[] holds the first mismatching row. No-op off the S3.
// `clockMultiplier` > 1 = a 74HCT595 expander is fitted: the private bus is built at the
// shift-mode pclk, and the GPIO CONTINUITY pre-check is SKIPPED. That pre-check drives the TX pin
// and expects the RX pin to follow directly: true for a bare jumper, false through a shift
// register (driving the serial input high does not raise an output; that takes 8 clocks + a latch),
// so it would report "jumper not detected" on perfectly good wiring. The captured signal is the
// real post-'595 WS2812 waveform, so the bit-verify itself is unchanged.
RmtLoopbackResult i80Ws2812Loopback(const uint16_t* dataPins, uint8_t laneCount,
                                    uint16_t wrGpio, uint16_t dcGpio, uint16_t rxGpio,
                                    const uint8_t* frame, size_t frameBytes,
                                    size_t dataBytes, uint8_t rowBits,
                                    uint8_t clockMultiplier = 1);

// ---------------------------------------------------------------------------
// MoonI80: the same i80 output, on OUR OWN DMA driver instead of IDF's esp_lcd.
//
// **Why a second implementation exists.** esp_lcd re-arms the peripheral on every
// transaction: `lcd_start_transaction()` does `lcd_ll_reset()` + `lcd_ll_fifo_reset()` +
// a hard-coded 4 µs busy-wait before each one. An LCD panel does not care; WS2812 is one
// unbroken self-clocked bit stream, so a mid-frame reset garbles everything after it.
// That makes a frame split across several esp_lcd transactions impossible to send gaplessly,
// at any chunk size: which in turn forces the whole frame into ONE transaction, and THAT is
// what caps the driver: the DMA must stream the entire frame from one contiguous, DMA-
// reachable block (hence ~96 lights/strand through the '595 expander on an S3, and no PSRAM
// at all on the classic ESP32).
//
// The hardware never demanded this. The LCD peripheral has no data-length register -
// `lcd_ll_set_phase_cycles()` only sets `lcd_dout` as a boolean enable, and IDF's own comment
// reads "Number of data phase cycles are controlled by DMA buffer length". So the peripheral
// clocks out exactly what the DMA feeds it and stops when the chain ends: ONE gdma_start() over
// an arbitrarily long descriptor chain + ONE lcd_ll_start() is a single gapless stream across as
// many buffers as we like. This backend takes that, built on IDF's HAL + GDMA link-list APIs
// (one level below esp_lcd: not raw registers; IDF's own drivers use the same APIs).
//
// **Both implementations ship.** The esp_lcd one above is the REFERENCE: correct, capped, and
// what this is measured against. Selecting between them is a module swap in the UI (two
// registered driver types), so the A/B needs no reflash. See docs/adr/0014.
//
// Identical contract to the i80Ws2812* family above, function for function: the domain driver
// (src/light/drivers/MoonLedDriver.h) is the same CRTP sibling with its forwards re-pointed.
// Inert on chips without LCD_CAM.
// ---------------------------------------------------------------------------

struct MoonI80Ws2812Handle { void* impl = nullptr; };

// **The streaming ring: how MoonI80 drives a frame too big to hold.**
//
// The whole-frame path above needs the entire encoded frame in one DMA-reachable block, and that is
// what caps the 74HCT595 expander: the encoder emits ~1,152 bytes per light in shift mode, so 96
// lights per strand is already 108 KB: the internal-DMA-RAM edge. Above that the frame can only live
// in PSRAM, and the S3's GDMA cannot sustain a PSRAM read at the expander's 10× pixel clock (measured:
// a PSRAM frame drives fine at 2.67 MHz and never completes at 26.67 MHz, at any size). Moving that
// read to the CPU does not help: same memory, same bus, and the CPU is not faster at bulk reads.
//
// So the frame is never materialised at all. The DMA loops a small ring of INTERNAL buffers, and as
// each one drains, the CPU encodes the next slice straight into it: reading the Layer buffer, which
// is internal and ~24× smaller than the encoded output (3 bytes/light vs 1,152). PSRAM leaves the path
// entirely. Espressif's RGB-LCD driver calls the same trick "bounce buffers"; hpwit's LED driver
// arrived at it independently.
//
// The deadline is comfortable, and the expander is *why*: the DMA takes ~345 µs to drain one 16-row
// buffer while the CPU encodes those rows in ~96 µs (measured on an S3): the 8× output inflation buys
// far more DMA time than it costs CPU, a ~3.6× margin.
//
// **The refill runs INLINE IN THE GDMA EOF ISR**: IDF's own continuous-gapless pattern (the RGB-LCD
// bounce buffers, esp_lcd_panel_rgb.c: the refill runs synchronously in the EOF handler). As each buffer
// drains, the ISR encodes the next slice into it at interrupt priority, so the refill always finishes
// before the DMA laps back into that buffer `kRingBufs` slices later. That is the reuse-race guarantee: a
// lower-priority task (the original design) could lose the buffer-reuse race to task-wake latency at >8
// slices (≥192 lights/strand), stalling the frame; an ISR cannot. The ring channel sets
// `isr_cache_safe = true` and the whole encode chain is IRAM-resident (MM_RAMFUNC): the shipped
// hardening, because a flash-cache miss inside a wire-rate ISR would blow the refill deadline. Being
// cache-safe, the ISR can fire while the flash cache is disabled (a SPI-flash write: OTA/NVS), so the
// refill DEFERS when `spi_flash_cache_enabled()` is false and the batch catches up afterward: never
// touching flash from the ISR. Prior art: esp_lcd_panel_rgb.c's ISR-refill under CONFIG_LCD_RGB_ISR_IRAM_SAFE.
//
// `MoonI80EncodeFn` is the seam: the platform owns the ring, the descriptors and the completion; the
// domain owns the encode. The callback runs from the EOF ISR (and once from the priming call).
//
// `needsPrefill` is the platform's buffer-lifecycle fact the encode's biggest saving hangs on: a ring
// buffer's CONSTANT words (the shift waveform frame prefillShiftRows lays) survive recycling: a data-only
// refill of a recycled buffer is byte-identical to a full one: so the encoder may skip the prefill except
// when the platform says the buffer's constants are gone: its FIRST use since the pool was built, or after
// any platform-side memset (the short-last-slice tail zero, the past-frame zero-fill). Only the platform
// knows those events, so it computes the flag; the domain decides what "prefill" means (and may still
// prefill unconditionally when its lane masks vary per row: ragged strands). Measured: the per-refill
// prefill was ~1/3 of the ISR encode cost.
// The FRAME-CLOSE call: `rowCount == 0 && closeFrame` asks the domain to write ONLY its frame-closing
// word (the shift expander's latch-only word) at `dst`: the platform makes this call for the first
// zero-lap slice past the frame, whose head then presents the register's final slot on the strand (a
// '595 output only changes when LATCHED, so a frame that ends without one more latch pulse leaves the
// strand frozen on its second-to-last slot through the reset). Encoders with no close word (direct
// mode) do nothing: the zeroed buffer is already a clean LOW.
using MoonI80EncodeFn = void (*)(void* user, uint8_t* dst, uint32_t firstRow, uint32_t rowCount,
                                 bool closeFrame, bool needsPrefill);

bool moonI80Ws2812Init(MoonI80Ws2812Handle& h, const uint16_t* dataPins, uint8_t laneCount,
                       uint16_t wrGpio, size_t bufferBytes,
                       bool wantSecondBuffer, uint8_t clockMultiplier = 1);

// Bring the bus up in RING mode instead of whole-frame mode. `rowBytes` is what one row (one light
// across every strand) encodes to, `totalRows` the strand length; the platform sizes the ring from
// them. `encode` is called per drained buffer, from the pinned refill task the EOF ISR wakes, to fill
// the next slice. Returns false if the ring cannot be built (then the caller falls back to whole-frame).
// The ring geometry the driver ships with, and the values its self-test uses. Named here (not duplicated
// per caller) so the driver's control defaults and the platform's own use cannot drift apart.
// The bench-tuned defaults: wall-verified clean at 256 lights/strand on 16 strands (rows=7 is the
// one-descriptor-node maximum at the 16-strand row size, so larger values clamp to it anyway; bufs=16
// rides the measured pool knee; the matching pad default lives on the driver's ringPadUs control).
constexpr uint8_t kRingRowsDefault = 7;    // lights per DMA buffer
constexpr uint8_t kRingBufsDefault = 16;   // buffers the DMA circulates
// Per-slice zero-pad ceiling, µs. A LOW gap under ~150 µs inside a WS2812 stream reads as a PAUSE, not a
// latch (the strand latches at ~300 µs measured), so an inter-buffer pad up to this bound stretches the
// refill deadline without ending the frame: hpwit's _DMA_EXTENSTION mechanism, sized to stay well under
// the latch threshold. The pad's fps cost is linear (frame += nSlices × padUs), which is why the value is
// a driver CONTROL bounded by this constant, not a platform constant applied unconditionally.
constexpr uint8_t kRingPadMaxUs = 120;
// Max bytes one GDMA descriptor node carries (LCD_DMA_DESCRIPTOR_BUFFER_MAX_SIZE). Shared here because
// BOTH sides derive geometry from it: the platform clamps a ring buffer to one node, and the driver's
// auto geometry computes the same rows-per-node ceiling to SHOW the user real values (see ringAuto).
constexpr size_t kRingNodeMaxBytes = 4095;
// The ring pool's depth bounds: shared for the same reason as kRingNodeMaxBytes: the platform enforces
// them (array bound / bounce floor) and the driver's auto geometry + control range must agree or drift.
// 64 keeps the prime-only regime (ringBufs ≥ nSlices: every slice encoded before arm, no ISR deadline)
// reachable at 48×256 (nSlices = 37 at 7 rows/slice); the pool arrays it bounds cost bytes, not KB.
constexpr uint8_t kRingBufsMax = 64;
constexpr uint8_t kRingBufsMin = 2;

// `rowsPerBuf` (lights per DMA buffer) and `ringBufs` (pool depth) are the ring's GEOMETRY, and they are
// the caller's choice because the optimum is a measurement, not a derivation. RAM is the only axis that
// wants a small rowsPerBuf: it alone stops scaling with strand length at 1 (the only way a 48x256 frame
// is reachable at all); per-call encode overhead, interrupt rate and lap-time runway all want it big.
// `padUs` (0..kRingPadMaxUs) inserts a shared zero-pad node after every buffer, stretching the per-slice
// refill deadline by that many µs at a linear frame-time cost; 0 = no pad nodes at all.
// Returns false (caller falls back to whole-frame) if the pool won't fit, if rowsPerBuf is 0, or if
// ringBufs is outside the platform's supported depth.
bool moonI80Ws2812InitRing(MoonI80Ws2812Handle& h, const uint16_t* dataPins, uint8_t laneCount,
                           uint16_t wrGpio, size_t rowBytes, uint32_t totalRows,
                           uint32_t rowsPerBuf, uint8_t ringBufs, uint8_t padUs,
                           uint8_t clockMultiplier, MoonI80EncodeFn encode, void* user);

// Start one frame on the ring: prime the buffers, fire the DMA, and let the refill task (woken by the
// EOF ISR) refill behind it. Pair with moonI80Ws2812Wait(h, 0, …): the ring reports completion on slot 0.
bool moonI80Ws2812TransmitRing(MoonI80Ws2812Handle& h);
// The dual-core split of TransmitRing: prime a SUB-RANGE of the pool's buffers (each independent, so two
// cores prime disjoint ranges concurrently), then arm once EVERYTHING is primed: the caller's join is
// the fence. TransmitRing remains the serial combo (prime all + arm) for the single-core path.
// Set the '595 shift-clock prescale off the 80 MHz bus resolution (4 = 20 MHz default: the reliability
// point; 3 = 26.67 MHz overclock; 5 = 16 MHz is past the WS2812 0-vs-1 threshold, all-white). A slower
// clock gives the shift register more setup margin on marginal strand wiring, at a longer WS2812 T0H.
// Takes effect on the next bus (re)build. See kShiftClockDivDefault in the i80 driver.
void moonI80SetShiftClockDiv(uint8_t div);
void moonI80Ws2812PrimeRange(MoonI80Ws2812Handle& h, uint8_t bufLo, uint8_t bufHi);
bool moonI80Ws2812ArmRing(MoonI80Ws2812Handle& h);

// True when the handle was brought up as a ring (so the driver knows which transmit to call).
bool moonI80Ws2812IsRing(const MoonI80Ws2812Handle& h);

// Would a whole `bytes`-sized frame fit internal DMA RAM right now (leaving the WiFi/HTTP reserve)? The
// driver asks this to decide RING vs whole-frame: a shift frame that fits internal takes the proven
// whole-frame path; one that doesn't would fall to PSRAM and stall at the expander clock, so it rings
// instead. Reuses the exact heap-caps check moonI80Ws2812Init does. False on chips without LCD_CAM.
bool moonI80Ws2812InternalFits(size_t bytes);
uint8_t* moonI80Ws2812Buffer(const MoonI80Ws2812Handle& h, uint8_t buffer);
size_t moonI80Ws2812BufferCapacity(const MoonI80Ws2812Handle& h);
bool moonI80Ws2812Transmit(MoonI80Ws2812Handle& h, uint8_t buffer, size_t bytes);
bool moonI80Ws2812Wait(MoonI80Ws2812Handle& h, uint8_t buffer, uint32_t timeoutMs);
uint32_t moonI80Ws2812LastTransmitUs(const MoonI80Ws2812Handle& h);

// Ring diagnostic counters, surfaced so the driver can expose them as a read-only control (reliable
// polling via /api/state instead of scraping serial). All zero on a whole-frame handle. `nSlices` is the
// frame's slice count (light-count / rowsPerBuf); `eofTotal`/`doneGiven` are lifetime counts the EOF ISR
// bumps; `lastDrain` is the drainCount the last EOF saw (should reach nSlices each frame); `numItems`/
// `consumedItems` are the descriptor pool capacity vs what the mount loop used (a mismatch is the ≥256
// chain-sizing bug). Read-only, best-effort (volatile reads, no lock): a diagnostic, not a contract.
struct MoonI80RingStats {
    bool     isRing = false;
    uint32_t nSlices = 0;
    uint32_t ringBufs = 0;       // kRingBufs (buffer-pool size; reuse happens when nSlices > this)
    uint32_t eofTotal = 0;       // lifetime EOF interrupts
    uint32_t doneGiven = 0;      // lifetime frame completions (last-slice EOF)
    uint32_t lastDrain = 0;      // drainCount at the last EOF
    uint32_t numItems = 0;       // descriptor pool capacity
    uint32_t consumedItems = 0;  // items the mount loop actually used (== numItems when sized right)
    uint32_t descErr = 0;        // GDMA descriptor-error count (>0 == the in-ISR encode corrupted the chain: B1)
    uint32_t maxEncodeUs = 0;    // worst ISR refill-encode time (the producer's JITTER number)
    uint32_t avgEncodeUs = 0;    // average refill-encode time (the producer's PACE number: the one that
                                 // decides whether the ring keeps up; the max only sizes the pool's margin)
    uint32_t maxIsrGapUs = 0;    // worst gap between EOFs = DMA buffer-drain time (the deadline)
    uint32_t late = 0;           // slices refilled AFTER the clock oracle said their drain began: each
                                 // one was stale on the wire. The machine's scatter meter: a clean soak
                                 // is late == 0; any increment is a deadline miss the eye may not catch.
    // Ring-diagnosis fields: the instruments that isolated the three prime-only bugs (mount re-link,
    // multi-node buffers, EOF coalescing); the lapping work reads them the same way. Their scope lives in
    // backlog-light § MoonI80 streaming ring.
    uint32_t itemsPerBuf = 0;    // descriptor nodes per ring buffer (1 by construction since the clamp)
    int32_t  termNodeDiag = -1;  // the mount-time NULL terminator node (-1 = looping/lapping chain)
    uint32_t cacheOffDefers = 0; // lifetime EOF firings that refilled NOTHING because the flash cache was
                                 // off (a flash/WiFi write). Expected background noise: the WiFi driver
                                 // toggles the cache ~10/s even at idle; benign now (see stallAbandons).
    uint32_t cacheOffMaxRun = 0; // worst run of CONSECUTIVE cache-off defers ≈ how many buffers the DMA
                                 // drained un-refilled during one write. When it exceeds the pool's lead the
                                 // DMA reaches the frontier terminator and HALTS (no stale replay): counted
                                 // as a stallAbandon, not corruption. High here + low stallAbandons = the
                                 // pool absorbed every window; high both = the walls will show a held frame.
    uint32_t stallAbandons = 0;  // lifetime frames finalized by the wait backstop because a cache-off window
                                 // outlasted the pool's lead and the DMA self-terminated at the frontier
                                 // (moonI80Ws2812Wait). Each = one partially-updated frame held for one
                                 // frame period: the DESIGNED benign outcome (vs. the old stale-slice burst).
};
MoonI80RingStats moonI80Ws2812RingStats(const MoonI80Ws2812Handle& h);

void moonI80Ws2812Deinit(MoonI80Ws2812Handle& h);
// `useRing` makes the self-test ride the ring exactly when the render path does, so it verifies the SAME
// transport the driver is tuned to (not "only when the frame won't fit internal"): the instrument the
// ring's margin bug needs. `ringRows`/`ringBufs` are that ring's geometry (0 → the platform default). The
// bit-verify then measures the ACTUAL ring: a margin the eyes see scattered on the wall shows here as a
// bit fault at the same slice boundary: the machine reproduction of the wall (the margin rule,
// `ring-reuse-is-the-blocker`). `useRing=false` keeps the legacy auto-gate (ring iff the frame overflows
// internal RAM), which is what direct-mode continuity callers want.
RmtLoopbackResult moonI80Ws2812Loopback(const uint16_t* dataPins, uint8_t laneCount,
                                        uint16_t wrGpio, uint16_t rxGpio,
                                        const uint8_t* frame, size_t frameBytes,
                                        size_t dataBytes, uint8_t rowBits,
                                        uint8_t clockMultiplier = 1,
                                        uint32_t ringRows = 0, uint32_t ringBufs = 0,
                                        bool useRing = false);

// INTRUSIVE loopback: DRIVER-AGNOSTIC, so it lives here (not per-family): bit-verify what the LIVE
// pipeline is ALREADY clocking on `rxGpio`, building no bus and leaving the running peripheral untouched
// (unlike the per-driver `*Loopback`, which tears the output down and rebuilds a private copy: a large
// contiguous alloc that fragments the heap and tests a replica). Because it only arms the RMT-RX (the
// render loop is the transmitter), it needs nothing driver-specific: every driver family (i80, esp_lcd,
// Parlio, RMT) shares this one entry, the same way they share `detail::captureAndVerifyFrame`. The caller
// pins a known per-light pattern (`sent`, `sentLen` channels) into the driver's source so the tapped
// strand's expected wire is deterministic. `dataBytes` = the tapped strand's WS2812 byte count (lights ×
// channels × 24 → kBits = dataBytes/3); `slotHz` = the STRAND's slot rate (bus rate ÷ expander multiplier,
// or the direct pixel-clock). A scattered ring shows as a bit fault at a slice boundary; a clean one PASSes.
RmtLoopbackResult ws2812LoopbackRide(uint16_t rxGpio, const uint8_t* sent, uint8_t sentLen,
                                     size_t dataBytes, uint8_t rowBits, uint8_t clockMultiplier);

// ---------------------------------------------------------------------------
// Parlio (Parallel IO) WS2812 output: the ESP32-P4's parallel LED path, a
// sibling of the LCD_CAM i80 functions above. Same autonomous-whole-frame DMA
// shape, but Parlio is simpler: it takes the data GPIOs directly (no
// sacrificial WR/DC lines: Parlio generates the pixel clock itself from
// `pclkHz`) and allows ANY lane count (1..8 here), so there is no all-8-pins
// rule. The same encoder feeds it (ParallelSlots.h: one bus word per slot, bit L =
// data line L). All inert on targets without Parlio, guarded by
// `if constexpr (platform::parlioLanes == 0)` in the driver.
// ---------------------------------------------------------------------------

// Opaque handle to one configured Parlio TX unit + one or TWO DMA frame buffers.
struct ParlioWs2812Handle { void* impl = nullptr; };

// Create a Parlio TX unit on `dataPins[0..laneCount)` clocked at `pclkHz` (the
// WS2812 slot rate), with a zeroed DMA-capable buffer 0 of `bufferBytes`. When
// `wantSecondBuffer` is true, also TRY a second buffer for the async double-buffer
// (same allocate-and-degrade contract as i80Ws2812Init); when false (default) no
// second buffer is allocated. No WR/DC pins: Parlio drives the clock internally.
// Returns false when buffer 0 (or the unit) fails.
bool parlioWs2812Init(ParlioWs2812Handle& h, const uint16_t* dataPins,
                      uint8_t laneCount, uint32_t pclkHz, size_t bufferBytes,
                      bool wantSecondBuffer);

// DMA frame buffer `buffer` (0 or 1; buffer 1 is null when it didn't fit) + the
// shared per-buffer capacity. See i80Ws2812Buffer for the single-buffer-degrade contract.
uint8_t* parlioWs2812Buffer(const ParlioWs2812Handle& h, uint8_t buffer);
size_t parlioWs2812BufferCapacity(const ParlioWs2812Handle& h);

// The most bytes Parlio can send in ONE transfer: a HARDWARE ceiling, not a heap budget, so it
// needs no handle and holds before anything is allocated. A caller sizes a frame against it to
// refuse an impossible configuration up front instead of failing the bus init.
//
// 0 means NO BOUND (the dmaBudgetBytes contract), not "zero bytes usable": it is what a host
// without Parlio returns, and what a caller reads as "nothing to check against".
size_t parlioMaxTransferBytes();

// Start the autonomous DMA transfer of buffer `buffer`'s first `bytes`; pair
// with parlioWs2812Wait on the SAME buffer. No refill deadline once started
// (single-shot, not the loop-transmission mode Parlio also offers).
bool parlioWs2812Transmit(ParlioWs2812Handle& h, uint8_t buffer, size_t bytes);

// Block until buffer `buffer`'s in-flight transfer finishes, bounded by `timeoutMs`.
// Returns TRUE only when the transfer actually completed; FALSE on timeout: see i80Ws2812Wait for
// why the caller must not reuse the buffer then.
bool parlioWs2812Wait(ParlioWs2812Handle& h, uint8_t buffer, uint32_t timeoutMs);

// Duration in microseconds of the most recent completed DMA transfer: the pure wire/DMA output
// time (the WS2812 floor / fps ceiling). See i80Ws2812LastTransmitUs. 0 until the first completes.
uint32_t parlioWs2812LastTransmitUs(const ParlioWs2812Handle& h);

void parlioWs2812Deinit(ParlioWs2812Handle& h);

// Parlio loopback self-test: same contract + result shape as the LCD/RMT
// loopbacks: a private Parlio TX unit transmits the caller's real frame back to
// back while rmtWs2812RxCapture reads it off `rxGpio` (lane 0 carries the
// pattern) and every bit is verified. `dataBytes`/`rowBits` as in i80Ws2812Loopback.
RmtLoopbackResult parlioWs2812Loopback(const uint16_t* dataPins, uint8_t laneCount,
                                       uint16_t rxGpio, const uint8_t* frame,
                                       size_t frameBytes, size_t dataBytes,
                                       uint8_t rowBits);

// ---------------------------------------------------------------------------
// I2S audio input (digital MEMS microphone, e.g. INMP441). Two seams only:
// the I2S read (audioMic*) and the FFT kernel (audioFft). Everything else -
// DC strip, RMS, windowing, the magnitude->16-band log mapping, noise-floor/gain -
// is host-tested domain code (src/core/AudioLevel.h, AudioBands.h), so the level
// and band math runs in CI without hardware. On desktop audioMicRead returns 0
// (no capture) but audioFft is a real (naive) DFT, so the whole
// read->window->FFT->bands path is still exercised host-side.
// All inert on targets without I2S, guarded by `if constexpr (platform::hasI2sMic)`.
// ---------------------------------------------------------------------------

// `CodecType` + `AudioCodecPins` are defined in platform_config.h (included above),
// alongside the per-target `audioCodecType`/`audioCodecPins` defaults that use them.
//
// Configure the audio codec (record/mic path) over I2C, if the board has one.
// Some boards put an analog mic behind an I2S audio codec (configured over I2C)
// rather than a direct digital I2S MEMS mic; the codec must be brought up before
// the I2S read. This is a no-op returning true on a board with a direct mic
// (CodecType::None) or a target without a codec, so AudioService always calls it and
// the path stays uniform.
// Returns true when there's nothing to do (CodecType::None / no codec on this
// target) or the codec came up; false on an I2C/codec error (the module then
// idles with a status error, same as a failed mic init). AudioService calls this
// *after* audioMicInit: the I2S channel must already be driving MCLK before the
// codec is configured (the ES8311 won't answer I2C without MCLK running). The
// codec then presents standard I2S the read picks up.
bool audioCodecInit(CodecType type, const AudioCodecPins& pins, uint32_t sampleRate);

void audioCodecDeinit();

// Opaque handle to one live audio input: an I2S RX channel on boards, an OS capture
// device on desktop. Both feed the same audioMicRead contract.
struct AudioMicHandle { void* impl = nullptr; };

// One live-audio gate for AudioService: a pin-wired I2S mic (boards) or an OS capture
// device (desktop, hasAudioCapture) both make the local analysis path run.
constexpr bool hasAudioInput = hasI2sMic || hasAudioCapture;

// OS capture devices (desktop; 0 where hasAudioCapture is false). Points optionsOut at a
// PLATFORM-OWNED stable string array: entry 0 is always "default" (the OS default capture
// device), entries 1..n-1 the named devices. Re-enumerated on each call, so hot-plugged
// devices (a BlackHole-style loopback installed while running) appear on the next call.
size_t audioCaptureDevices(const char* const** optionsOut);

// Open capture device `deviceIndex` (an index into the last audioCaptureDevices list; 0 =
// default) as mono 24-bit-left-justified samples at `sampleRate` (the backend resamples).
// False on failure (no device, OS permission denied): degrade, never crash.
bool audioCaptureInit(AudioMicHandle& h, uint8_t deviceIndex, uint32_t sampleRate);

// Bring up an I2S RX channel reading the mic on the given pins at `sampleRate`
// (24-bit data in a 32-bit slot, mono). `mclkPin` drives the I2S master clock -
// −1 for a self-clocked direct MEMS mic (INMP441), or the codec's MCLK pin when a
// codec needs the clock to run (the ES8311 won't even answer I2C without it, so
// AudioService starts I2S *before* audioCodecInit on a codec board). Returns false
// on failure (bad pins, no I2S, out of memory): the module idles with a status error.
/// How the microphone speaks, which decides how many pins it needs and how the peripheral is
/// configured. Two physically different parts, not two settings of one:
///   - `I2sStd`: a PCM part (INMP441 and friends) on three wires, bit clock + word select + data,
///     already-decoded samples in Philips framing.
///   - `Pdm`: a one-bit-stream part on TWO wires, clock + data, decimated to PCM by the
///     peripheral. Boards with a mic soldered on tend to use these because they are cheaper and
///     smaller: the QuinLED Dig-Next-2's onboard mic is one (clock GPIO 8, data GPIO 7).
/// `sckPin` and `mclkPin` are meaningless in PDM mode and ignored.
enum class MicMode : uint8_t { I2sStd = 0, Pdm = 1 };

bool audioMicInit(AudioMicHandle& h, uint16_t wsPin, uint16_t sdPin,
                  uint16_t sckPin, int16_t mclkPin, uint32_t sampleRate,
                  MicMode mode = MicMode::I2sStd);

// Read up to `maxSamples` 32-bit samples into `out`; returns the count read
// (0 if none ready / not initialized). Non-blocking enough for the render tick.
size_t audioMicRead(AudioMicHandle& h, int32_t* out, size_t maxSamples);
// Whether the I2S instance a PDM microphone needs is free right now. The mirror of
// i80Ws2812SharedBusFree: on the classic ESP32 the parallel LED bus is that same instance, so a
// microphone refused at claim time polls this to come up once the bus lets go. False where nothing
// is shared (every chip whose i80 is LCD_CAM, and the desktop). Cheap: a registry read, no init.
bool audioMicSharedBusFree(MicMode mode);

void audioMicDeinit(AudioMicHandle& h);

// Real-input FFT kernel: `windowed` holds `n` (a power of two) windowed samples;
// fills `outMag` with the n/2 magnitude bins. esp-dsp's float `dsps_fft2r_fc32`
// on ESP32 (the FPU makes float faster than fixed-point); a naive O(n^2) DFT on
// desktop: correct, only fast enough for the host tests' small n.
void audioFft(const float* windowed, size_t n, float* outMag);

// ---------------------------------------------------------------------------
// I2C bus diagnostics: domain-neutral, not audio-specific. Probes a bus and
// reports which 7-bit addresses ACK, the standard `i2cdetect` operation. Used
// by the I2cScanModule diagnostic (src/core/I2cScanModule.h) to help bring up
// any I2C peripheral (a codec, a sensor, an expander): confirm wiring and read
// off a device's address. Self-contained: opens a temporary master bus on the
// given pins, scans, tears it down. The bus is transient, so it only conflicts
// with a driver that *currently* holds the port (e.g. the ES8311 codec keeps
// I2C_NUM_0 open while AudioService is active): that case is reported as
// kI2cBusUnavailable, not silently as "0 devices". Internal pull-ups enabled.
// ---------------------------------------------------------------------------

// Sentinel: the bus couldn't be opened (already held by another driver, or no
// I2C on this target): distinct from a successful scan that found 0 devices.
inline constexpr size_t kI2cBusUnavailable = static_cast<size_t>(-1);

// Scan the I2C bus on (sda, scl); write the 7-bit addresses that ACK into
// `out` (caller-sized, capacity `maxOut`) and return the count found (capped at
// maxOut), or kI2cBusUnavailable if the bus couldn't be opened.
size_t i2cScan(uint16_t sda, uint16_t scl, uint8_t* out, size_t maxOut);

// --- GPIO as an input/output ROLE ------------------------------------------------------------
// The pair above (gpioCapability / gpioLiveState) answers "what is this pin, and what is it doing"
// for the pin map: a diagnostic, sampled on tick1s. These three are the working seam a module uses
// to actually READ a switch or DRIVE a line, which nothing could do before: a button, a foot pedal,
// a relay enable. Deliberately minimal and synchronous, matching irRead's shape - the module owns
// debouncing and edge detection, because a bouncing contact is a property of the switch, not of the
// platform.

/// The internal pull to enable on an input. A mechanical switch needs one; a pin driven by another
/// device usually does not.
enum class GpioPull : uint8_t { None = 0, Up, Down };

/// Configure one GPIO as an input, with an optional internal pull. Idempotent: re-configuring a pin
/// the caller already owns is not an error, so a live pin-control change just re-runs it. Returns
/// false when `gpio` is not a usable input on this chip (gpioCapability().validGpio is false).
/// Desktop accepts any pin and reads back what setTestGpioLevel injected, so button logic is
/// host-testable with no hardware.
bool gpioInputBegin(uint8_t gpio, GpioPull pull);

/// Read a pin configured by gpioInputBegin: true = HIGH. Cheap enough for a per-tick poll (one
/// register read on ESP32); no allocation, never blocks. An unconfigured pin reads false.
///
/// RAW: the level as the pad reads it, deliberately unfiltered. The ESP32 has a hardware glitch
/// filter, and this seam does not use it, because debouncing is a property of the SWITCH rather than
/// of the pin: the time constant belongs to the module that knows what is wired (ButtonService owns
/// it, in milliseconds it can explain). A filter here would be a second, invisible one underneath,
/// tuned in clock cycles, that no test could reach and no other platform could reproduce.
bool gpioRead(uint8_t gpio);

/// Drive one GPIO as a push-pull output. Configures it on first use, so no separate begin: a caller
/// that owns the pin just writes it. Returns false when the pin has no output driver
/// (gpioCapability().outputCapable is false, e.g. classic ESP32 34-39). This is what a relay enable
/// needs, and what MoonLive's "write to gpio" builtin will call.
bool gpioWrite(uint8_t gpio, bool high);

/// Test-only (desktop): make gpioRead(gpio) return `level`. Same shape as setTestGpioCapability.
void setTestGpioLevel(uint8_t gpio, bool level);
void clearTestGpioLevel();

/// Read one ADC pin. Returns false when `gpio` has no ADC on this chip, or the read failed.
///
/// RAW COUNTS, deliberately, in the chip's own resolution (0..4095 on ESP32 at 12 bits). Not
/// millivolts: everything this seam exists for maps a travel to a range anyway (a pedal's usable
/// throw is never the full sweep, so `AnalogService` carries min/max/invert per row), and a
/// millivolt figure would add per-chip calibration machinery to serve a conversion the caller
/// immediately undoes. A sensor that reports an actual voltage can have `adcReadMv` the day one
/// exists; inventing it first would be a seam with no caller.
///
/// Configures the pin on first use, so there is no separate begin: the same shape `gpioWrite` uses,
/// and for the same reason (a caller that owns the pin just reads it). Cheap enough for a per-tick
/// poll and never blocks.
///
/// Unfiltered, exactly as `gpioRead` is: a potentiometer's jitter is a property of the pot and the
/// wiring, so smoothing belongs to the module that knows what is connected. `AnalogService` owns
/// its own, in a time constant it can explain.
bool adcRead(uint8_t gpio, uint16_t& raw);

/// The full-scale count `adcRead` reports on this platform, so a caller can scale without knowing
/// the chip: 4095 at the ESP32's 12 bits. One number rather than a bit-depth, because a caller
/// scaling a range wants the maximum, not the exponent that produced it.
uint16_t adcMaxCount();

/// Read one ADC pin as MILLIVOLTS. Returns false where `adcRead` would, or where the chip carries
/// no calibration data.
///
/// The counterpart of `adcRead`, and NOT a convenience over it: the raw count is not a fixed
/// fraction of full scale, because every chip's converter is nonlinear in its own measured way. The
/// ESP32 stores per-chip correction in eFuse and the IDF applies it, so this is the only way to get
/// a figure that means volts rather than "counts on this particular part".
///
/// The caller that needs this is a sensor whose reading is a VOLTAGE: a divider measuring the
/// supply rail, a shunt amplifier reporting current. Scaling those from raw counts gives a number
/// that looks plausible and is wrong, which is worse than refusing. A pedal or a pot needs no such
/// thing and should keep using `adcRead`: it maps a travel to a range, and a calibrated voltage
/// would be converted straight back out again.
bool adcReadMv(uint8_t gpio, uint16_t& mv);

/// Test-only (desktop): make adcReadMv(gpio) return `mv`.
void setTestAdcMv(uint8_t gpio, uint16_t mv);

/// Test-only (desktop): make adcRead(gpio) return `raw`. The counterpart of setTestGpioLevel, so a
/// pedal's mapping is host-testable with no hardware.
void setTestAdcValue(uint8_t gpio, uint16_t raw);
void clearTestAdcValue();

// Poll the IR receiver on `pin` for a decoded remote frame. Returns true and writes the
// frame into `codeOut` when a fresh code is available since the last call, false otherwise
// (nothing received, or IR decode unavailable on this target). Self-contained like i2cScan -
// it owns whatever peripheral it needs (an RMT RX channel on ESP32). Non-blocking: safe to
// call every tick. InfraredService is the sole caller. ESP32 decodes NEC over RMT; desktop has no IR
// hardware and always returns false.
bool irRead(uint16_t pin, uint32_t& codeOut);

// Release the IR RX channel so the pin it held is free for another module. irRead lazily reopens
// it on the next call, so this is safe to call any time; InfraredService calls it on disable (the pin
// then shows as freed in the pin map, and is genuinely reusable). No-op if no channel is open, and
// on desktop (no IR hardware).
void irStop();

// Open (or confirm) the IR RX channel on `pin` and report whether it's live: the difference between
// "a pin is configured" (which irRead can't distinguish from "no code this tick") and "the RMT-RX
// channel actually bound and is armed". InfraredService calls this to give a truthful status: a busy pin or
// a bad GPIO fails to open, and the user must see that, not a stale "ready". Idempotent for an
// unchanged pin (reuses the open channel). Returns true on ESP32 when the channel is live; desktop
// has no IR hardware, so it returns true (no channel to fail: the desktop status stays "ready").
bool irChannelReady(uint16_t pin);

} // namespace mm::platform
