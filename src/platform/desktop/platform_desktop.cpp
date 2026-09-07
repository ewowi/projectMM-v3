#include "platform/platform.h"
#include "core/FirmwareImage.h"  // identify/moonBaseRejection: shared image vetting

#include <algorithm>
#include <chrono>
#include <bit>       // std::countr_zero, the radix-2 audioFft's bit-reversal
#include <cmath>     // cos/sin/sqrt for audioFft's twiddles and magnitudes
#include <numbers>   // std::numbers::pi_v, same kernel
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#ifndef _WIN32
#include <dlfcn.h>   // dlopen/dlsym — the NDI runtime is resolved on demand, never linked
#endif
#include <vector>   // HostBus frame buffers — the memory-backed parallel bus
#include <thread>
#include <deque>     // encoder frame queue between the render tick and the writer thread
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <cerrno>

#ifdef _WIN32
// Winsock + Win32 socket APIs. SOCKET is an unsigned handle (INVALID_SOCKET = ~0),
// but `fd_` stays `int` in the cross-platform header — the narrowing is well-defined
// for handle values in the practical range and is the standard Win32 pattern.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <io.h>     // _fileno, _commit (POSIX fileno/fsync equivalents)
#include <iphlpapi.h>   // GetIfTable2 — real link state + negotiated speed (ethLinkUp)
#include <netioapi.h>   // MIB_IF_ROW2: sees a NIC a Hyper-V vSwitch hides from GetAdaptersAddresses
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>      // getaddrinfo — hostname resolution for TcpConnection::connect
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>   // waitpid: reaping the spawned ffmpeg (encoderRunning/Stop)
#include <spawn.h>      // posix_spawn: fd-hygienic, thread-safe child creation (encoderStart)
extern char** environ;   // posix_spawnp wants the environment explicitly
#include <csignal>      // SIGPIPE ignore + SIGKILL: the encoder pipe's failure surface
#include <sys/mman.h>   // mmap/munmap for allocExec (executable pages)
#include <net/if.h>     // if_nametoindex / ifreq — naming the NIC for raw L2 send
#include <ifaddrs.h>    // getifaddrs: the raw-interface Select enumerates the host NICs
#ifdef __linux__
#include <netpacket/packet.h>   // sockaddr_ll — AF_PACKET raw frames (ethSendRaw)
#include <net/ethernet.h>       // ETH_P_ALL
#endif
#ifdef __APPLE__
#include <net/if_media.h>   // SIOCGIFMEDIA: the negotiated link rate, for the interface labels
#include <pthread.h>    // pthread_jit_write_protect_np — macOS arm64 W^X JIT toggle
#include <sys/ioctl.h>  // BIOCSETIF — binding a BPF device to an interface (ethSendRaw)
#include <net/bpf.h>
#endif
#endif

namespace mm::platform {

namespace {
/// Append ", <speed>" to an interface label, in the one format every OS's list uses.
///
/// The speed LOOKUP is necessarily per-OS (MIB_IF_TABLE2 on Windows, sysfs on Linux, SIOCGIFMEDIA
/// on macOS: three APIs, three units), which is what the platform layer is for. The RENDERING is
/// not, so it lives here once: the label shape is a contract the apply path and the driver's remap
/// both parse (they split on ", " to recover the adapter's stable identity), and two copies of it
/// would be two chances to drift out of that agreement.
///
/// `mbps` of 0 means the OS would not state a speed (a virtual adapter, a link that is down, or
/// macOS Wi-Fi reporting only "autoselect"). That appends nothing, rather than a fabricated
/// "0 Mb". Appends only if the whole suffix fits: a truncated speed reads worse than none, and
/// the label is what the Select persists by.
void appendLinkSpeed(char* out, size_t cap, unsigned mbps) {
    if (!out || mbps == 0) return;
    const size_t n = std::strlen(out);
    char speed[24];
    if (mbps >= 1000 && mbps % 1000 == 0)
        std::snprintf(speed, sizeof(speed), ", %u Gb", mbps / 1000);
    else if (mbps >= 1000)
        std::snprintf(speed, sizeof(speed), ", %u.%u Gb", mbps / 1000, (mbps % 1000) / 100);
    else
        std::snprintf(speed, sizeof(speed), ", %u Mb", mbps);
    if (n + std::strlen(speed) + 1 <= cap) std::snprintf(out + n, cap - n, "%s", speed);
}


// Tiny portability shims so each call site reads as plain code, not `#ifdef` noise.
// POSIX uses int FDs + errno + read/write/close; Winsock uses SOCKET handles +
// WSAGetLastError + recv/send/closesocket. Map to a small common surface.
#ifdef _WIN32
// SOCKET is unsigned (UINT_PTR). `sock(fd)` casts to it at API boundaries so
// /W4 doesn't warn about signed→unsigned at every call site.
inline SOCKET sock(int fd) { return static_cast<SOCKET>(fd); }
inline int close_sock(int fd) { return ::closesocket(sock(fd)); }
// WSAEWOULDBLOCK: non-blocking call had no buffer/data. WSAETIMEDOUT: blocking
// recv hit SO_RCVTIMEO without data. Both translate to POSIX EAGAIN semantics
// (the read/write path returns -1 / WouldBlock and the caller retries).
inline bool sockWouldBlock() {
    int err = ::WSAGetLastError();
    return err == WSAEWOULDBLOCK || err == WSAETIMEDOUT;
}
inline int open_sock(int domain, int type, int protocol) {
    SOCKET s = ::socket(domain, type, protocol);
    return (s == INVALID_SOCKET) ? -1 : static_cast<int>(s);
}
inline int make_nonblocking(int fd) {
    u_long mode = 1;
    return ::ioctlsocket(sock(fd), FIONBIO, &mode);
}
inline int make_blocking(int fd) {
    u_long mode = 0;
    return ::ioctlsocket(sock(fd), FIONBIO, &mode);
}
#else
inline int sock(int fd) { return fd; }
inline int close_sock(int fd) { return ::close(fd); }
inline bool sockWouldBlock() { return errno == EAGAIN || errno == EWOULDBLOCK; }
inline int open_sock(int domain, int type, int protocol) {
    return ::socket(domain, type, protocol);
}
inline int make_nonblocking(int fd) {
    int flags = ::fcntl(fd, F_GETFL, 0);
    return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}
inline int make_blocking(int fd) {
    int flags = ::fcntl(fd, F_GETFL, 0);
    return ::fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
}
#endif
#ifdef _WIN32
// Winsock 2.2 must be initialized once per process before any socket call.
// A static RAII guard runs at library load (covers both the app and the test
// binaries, which have their own main() but link mm_platform). WSAStartup is
// reference-counted so this is safe alongside any future caller-side init.
struct WinsockInit {
    WinsockInit() {
        WSADATA d;
        ::WSAStartup(MAKEWORD(2, 2), &d);
    }
    ~WinsockInit() { ::WSACleanup(); }
};
static WinsockInit g_winsockInit;
#endif

}  // namespace

static auto startTime = std::chrono::steady_clock::now();
// Test-only override for millis(); 0 means "use the real clock". std::atomic so
// a test can set it from one thread while a tested module reads from another.
static std::atomic<uint32_t> testNowMs{0};

void setTestNowMs(uint32_t ms) { testNowMs.store(ms, std::memory_order_relaxed); }

// steady_clock::now() is a vDSO clock_gettime read — no allocation, no lock, no syscall on
// any platform we build for. libc++ does not annotate it, so -Wfunction-effects has to assume
// the worst; this is the standard-library gap, not ours. Scoped to the two clock readers, and
// desktop-only (the ESP32 millis/micros call esp_timer_get_time directly).
// Ask the compiler whether it HAS the warning, rather than inferring it from a version number.
// `#pragma clang ...` is an unknown pragma to GCC, and `-Wfunction-effects` is an unknown warning
// group to older clangs — both are errors under -Werror. A `__clang_major__ >= 20` test does NOT
// work here: Apple Clang carries its own version line, so the macos-14 runner reports a major
// >= 20 while predating the warning, which is exactly how this reached main.
#if defined(__clang__) && defined(__has_warning)
#  if __has_warning("-Wfunction-effects")
#    define MM_SUPPRESS_FUNCTION_EFFECTS 1
#  endif
#endif
#ifdef MM_SUPPRESS_FUNCTION_EFFECTS
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wfunction-effects"
#endif
uint32_t millis() MM_NONBLOCKING {
    uint32_t override_ = testNowMs.load(std::memory_order_relaxed);
    if (override_) return override_;
    auto now = std::chrono::steady_clock::now();
    return static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now - startTime).count()
    );
}

// The OS thread identity as an integer. std::this_thread::get_id() is the portable spelling but is
// not convertible to an integer, so each platform's own call is used: GetCurrentThreadId on Windows,
// pthread_self elsewhere. The +1 guarantees a non-zero result so callers can treat 0 as "none".
uintptr_t currentThreadId() MM_NONBLOCKING {
#ifdef _WIN32
    return static_cast<uintptr_t>(GetCurrentThreadId()) + 1;
#else
    return reinterpret_cast<uintptr_t>(pthread_self()) + 1;
#endif
}

uint32_t micros() MM_NONBLOCKING {
    auto now = std::chrono::steady_clock::now();
    return static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(now - startTime).count()
    );
}
#ifdef MM_SUPPRESS_FUNCTION_EFFECTS
#pragma clang diagnostic pop
#endif

void* alloc(size_t bytes) {
    return std::malloc(bytes);
}

bool ptrIsPsram(const void* /*p*/) { return false; }   // desktop has no PSRAM

void* allocInternal(size_t bytes) {
    return std::malloc(bytes);   // desktop has one flat RAM — internal == ordinary
}

void free(void* ptr) {
    std::free(ptr);
}

// Executable memory for MoonLive's emitted code. macOS on Apple Silicon enforces W^X
// (a page is writable OR executable, never both at once) and demands MAP_JIT for any
// JIT page; the write happens later in writeExec, bracketed by a per-thread
// write-protect toggle. Linux/Windows allow a plain RWX page. Returns nullptr on
// failure so the caller degrades.
void* allocExec(size_t bytes) {
    if (bytes == 0) return nullptr;
#ifdef _WIN32
    void* p = VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    return p;   // VirtualAlloc returns nullptr on failure
#elif defined(__APPLE__)
    void* p = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE | PROT_EXEC,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_JIT, -1, 0);
    return p == MAP_FAILED ? nullptr : p;
#else
    void* p = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE | PROT_EXEC,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? nullptr : p;
#endif
}

void freeExec(void* ptr, size_t bytes) {
    if (!ptr) return;
#ifdef _WIN32
    (void)bytes;
    VirtualFree(ptr, 0, MEM_RELEASE);
#else
    ::munmap(ptr, bytes);
#endif
}

void writeExec(void* dst, const void* src, size_t len) {
    if (!dst || !src || !len) return;
#if defined(_WIN32)
    // Windows: the VirtualAlloc page is RWX; memcpy suffices, then FlushInstructionCache
    // (MSVC has no __builtin___clear_cache).
    std::memcpy(dst, src, len);
    FlushInstructionCache(GetCurrentProcess(), dst, len);
#elif defined(__APPLE__)
    // macOS arm64 W^X: flip this thread's MAP_JIT pages to writable, copy, flip back to
    // executable, then sync the I-cache (required on arm64 for freshly-written code).
    pthread_jit_write_protect_np(0);
    std::memcpy(dst, src, len);
    pthread_jit_write_protect_np(1);
    __builtin___clear_cache(static_cast<char*>(dst), static_cast<char*>(dst) + len);
#else
    // Linux: the RWX page is plain memory; memcpy suffices. arm64 Linux still wants an
    // I-cache sync; on x86-64 __builtin___clear_cache is a harmless no-op.
    std::memcpy(dst, src, len);
    __builtin___clear_cache(static_cast<char*>(dst), static_cast<char*>(dst) + len);
#endif
}

void yield() {
    // Hand the CPU to another runnable thread — the desktop twin of the ESP32's vTaskDelay(1).
    // It must actually yield, not no-op: the multicore split's frame boundary polls this while it
    // waits for the encode worker, and a no-op turns that into a busy-spin that pins a core and
    // starves the very worker it is waiting for. std::this_thread::yield() is the portable form.
    std::this_thread::yield();
}

void delayMs(uint32_t ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

void delayUs(uint32_t us) {
    std::this_thread::sleep_for(std::chrono::microseconds(us));
}

void pauseLoop() {
    // yield() alone only offers the CPU to another RUNNABLE thread, so on an otherwise idle
    // machine it returns at once and the caller spins a core flat out (reported from a Linux
    // bench as the process "slowly eating more cpu cycles ... maxed out one core").
    //
    // Sleep to a frame BUDGET rather than a fixed nap. Nothing consumes a desktop render faster
    // than a display or a driver's own fps limit, so a loop free-running at 2000+ FPS is spending
    // a core to compute frames no one reads. 4 ms is 250 FPS: far above any output rate we drive,
    // while leaving the CPU idle in between. A tick that legitimately takes longer than the budget
    // simply gets no sleep, so a heavy grid still runs as fast as it can.
    static constexpr auto kFrameBudget = std::chrono::microseconds(4000);
    static auto lastWake = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    const auto spent = now - lastWake;
    if (spent < kFrameBudget) std::this_thread::sleep_for(kFrameBudget - spent);
    lastWake = std::chrono::steady_clock::now();
}

size_t freeHeap() {
    return 0; // Not meaningful on desktop (0 = unlimited)
}

size_t freeInternalHeap() {
    return 0; // Not meaningful on desktop (0 = unlimited)
}

// Test-only cap on the reported largest-free block; 0 = unlimited (the real
// desktop default). Lets a test force MappingLUT's paged fallback (which only
// triggers when no single contiguous block fits) without an actual fragmented
// heap. std::atomic to match setTestNowMs's cross-thread contract.
static std::atomic<size_t> testMaxBlock{0};
void setTestMaxAllocBlock(size_t bytes) { testMaxBlock.store(bytes, std::memory_order_relaxed); }

size_t maxAllocBlock() {
    return testMaxBlock.load(std::memory_order_relaxed); // 0 = unlimited
}

size_t maxInternalAllocBlock() {
    return 0; // Not meaningful on desktop (0 = unlimited)
}

// No RTOS on desktop — the TasksModule shows only its MoonModule cost table here.
// Test seam: a unit test can inject a canned task snapshot + render-task name so TasksModule's
// row/detail JSON + the nesting predicate are exercised on the host (no RTOS here otherwise). Empty
// by default → the real "desktop shows no tasks" behaviour. Declared in platform.h under a test guard.
static const TaskInfo* g_testTasks = nullptr;
static size_t g_testTaskCount = 0;
static const char* g_testRenderTask = "";
void setTestTaskSnapshot(const TaskInfo* tasks, size_t count, const char* renderTask) {
    g_testTasks = tasks; g_testTaskCount = count; g_testRenderTask = renderTask ? renderTask : "";
}
size_t taskSnapshot(TaskInfo* out, size_t maxTasks) {
    if (!g_testTasks || !out) return 0;
    const size_t n = g_testTaskCount < maxTasks ? g_testTaskCount : maxTasks;
    for (size_t i = 0; i < n; i++) out[i] = g_testTasks[i];
    return n;
}
void currentTaskOnCore(int, char* out, size_t cap) { if (out && cap) out[0] = '\0'; }
const char* renderTaskName() { return g_testRenderTask; }

// Worker-task seam — std::thread + condition_variable backing. The `core` pin is ignored (the host
// has no core-affinity story; the core-split is ESP32-only), but the spawn/notify/wait/stop handoff
// is real, so the render↔encode invariants are host-testable on an actual second thread. The wake is
// a single-slot latch (`pending`): notifyTask sets it, waitNotify consumes it — matching the FreeRTOS
// direct-to-task notification's "one pending count" semantics so a host test sees the same behavior.
namespace {
struct DesktopWorker {
    std::thread thread;
    std::mutex mtx;
    std::condition_variable cv;
    bool pending = false;   // a notify is waiting to be consumed (the single-slot latch)
    bool stop = false;
};
}  // namespace

bool spawnPinnedTask(WorkerTask& t, const char* /*name*/, WorkerFn fn, void* user,
                     size_t /*stackBytes*/, uint8_t /*priority*/, int /*core*/) {
    auto* w = new (std::nothrow) DesktopWorker();
    if (!w) return false;
    t.impl = w;
    w->thread = std::thread([fn, user] { fn(user); });   // the fn owns its loop until stop
    return true;
}

void notifyTask(WorkerTask& t) {
    auto* w = static_cast<DesktopWorker*>(t.impl);
    if (!w) return;
    { std::scoped_lock<std::mutex> lk(w->mtx); w->pending = true; }
    w->cv.notify_one();
}

bool waitNotify(WorkerTask& t, uint32_t timeoutMs) {
    auto* w = static_cast<DesktopWorker*>(t.impl);
    if (!w) return false;
    std::unique_lock<std::mutex> lk(w->mtx);
    const bool got = w->cv.wait_for(lk, std::chrono::milliseconds(timeoutMs),
                                    [w] { return w->pending || w->stop; });
    if (!got) return false;         // timed out with no notify/stop
    w->pending = false;             // consume the single-slot latch
    return true;                    // woken by a notify OR stop; the fn re-checks its stop flag
}

void stopPinnedTask(WorkerTask& t) {
    auto* w = static_cast<DesktopWorker*>(t.impl);
    if (!w) return;
    { std::scoped_lock<std::mutex> lk(w->mtx); w->stop = true; }
    w->cv.notify_one();
    if (w->thread.joinable()) w->thread.join();
    delete w;
    t.impl = nullptr;
}

void taskWdtSubscribe() {}     // no watchdog on the host
void taskWdtUnsubscribe() {}   // no watchdog on the host
void taskWdtReset() {}         // no watchdog on the host


// A host build has no real GPIOs to protect — every pin is valid, output-capable, and free of
// straps/reserved roles. So the pin map on desktop flags nothing (which is correct: there's no
// silicon to corrupt). The ESP32 build fills the real capability in platform_esp32_gpio.cpp.
// A test can override one pin's capability (setTestGpioCapability) to exercise PinsModule's severity
// derivation on the host; a small fixed table (no heap) holds the overrides.
namespace {
struct GpioCapOverride { uint8_t gpio; GpioCapability cap; bool set; };
GpioCapOverride g_gpioCapOverrides[16] = {};
}  // namespace
GpioCapability gpioCapability(uint8_t gpio) {
    for (const auto& o : g_gpioCapOverrides)
        if (o.set && o.gpio == gpio) return o.cap;
    return GpioCapability{};
}
void setTestGpioCapability(uint8_t gpio, GpioCapability cap) {
    for (auto& o : g_gpioCapOverrides)
        if (!o.set || o.gpio == gpio) { o = {gpio, cap, true}; return; }
}
void clearTestGpioCapability() {
    for (auto& o : g_gpioCapOverrides) o.set = false;
}

// Live state — desktop has no real pins, so valid=false (the map omits the live columns) unless a
// test injects one. Same small fixed override table as the capability stub.
namespace {
struct GpioLiveOverride { uint8_t gpio; GpioLiveState state; bool set; };
GpioLiveOverride g_gpioLiveOverrides[16] = {};
}  // namespace
GpioLiveState gpioLiveState(uint8_t gpio) {
    for (const auto& o : g_gpioLiveOverrides)
        if (o.set && o.gpio == gpio) return o.state;
    return GpioLiveState{};   // valid=false
}
void setTestGpioLiveState(uint8_t gpio, GpioLiveState state) {
    for (auto& o : g_gpioLiveOverrides)
        if (!o.set || o.gpio == gpio) { o = {gpio, state, true}; return; }
}
void clearTestGpioLiveState() {
    for (auto& o : g_gpioLiveOverrides) o.set = false;
}

size_t totalHeap() {
    return 0; // Not meaningful on desktop
}

size_t totalInternalHeap() {
    return 0; // Not meaningful on desktop
}

void getMacAddress(uint8_t mac[6]) {
    // Stable fake MAC for desktop (consistent deviceName across runs)
    mac[0] = 0xDE; mac[1] = 0xAD; mac[2] = 0xBE;
    mac[3] = 0xEF; mac[4] = 0xCA; mac[5] = 0xFE;
}

const char* macString() {
    static char buf[18] = {};
    if (buf[0] == 0) {
        uint8_t mac[6];
        getMacAddress(mac);
        std::snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
                      mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
    return buf;
}

const char* chipModel() {
    return "desktop";
}

uint8_t currentCore() { return 0; }

uint32_t cycleCount() {
    return static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

const char* cpuInfo() {
    // Cores only: the host's clock speed has no portable query (and boosts dynamically anyway).
    static char buf[16] = {};
    if (!buf[0])
        std::snprintf(buf, sizeof(buf), "%u cores", std::thread::hardware_concurrency());
    return buf;
}

const char* hostIp() {
    // Resolve the outbound-interface address. A UDP socket connect() sends no
    // packet — it just selects the route — so getsockname() then yields this
    // host's LAN IP. Cached after the first call. "" if offline.
    static char ip[INET_ADDRSTRLEN] = {};
    if (ip[0]) return ip;
    int fd = open_sock(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return "";
    sockaddr_in probe{};
    probe.sin_family = AF_INET;
    probe.sin_port = htons(80);
    inet_pton(AF_INET, "8.8.8.8", &probe.sin_addr);
    if (::connect(sock(fd), reinterpret_cast<sockaddr*>(&probe), sizeof(probe)) == 0) {
        sockaddr_in local{};
        socklen_t len = sizeof(local);
        if (::getsockname(sock(fd), reinterpret_cast<sockaddr*>(&local), &len) == 0) {
            inet_ntop(AF_INET, &local.sin_addr, ip, sizeof(ip));
        }
    }
    close_sock(fd);
    return ip;
}

const char* sdkVersion() {
#ifdef __clang__
    return "clang " __clang_version__;
#elif defined(__GNUC__)
    return "gcc " __VERSION__;
#else
    return "unknown";
#endif
}

const char* coprocessorWifi() {
    return "";   // desktop has no WiFi co-processor
}

const char* psramType() {
    return "";   // desktop has no PSRAM
}

const char* resetReason() {
    // Desktop has no reset-reason concept; report a benign value the UI treats as "not crashed".
    return "OK";
}

void setLogLevel(LogLevel) {
    // Desktop logs to the terminal unconditionally; the KPI-line gate reads the level directly,
    // so there is nothing to apply to a platform logger here.
}

size_t firmwareSize() { return 0; }
size_t firmwarePartition() { return 0; }
size_t flashChipSize() { return 0; }

// Filesystem: std::filesystem rooted at fsRoot_. A leading '/' in the API path maps to
// root-relative.
//
// The root is a PER-USER data directory, not a path relative to wherever the process happened to
// start. A shipped binary is launched from a download folder, a Start-menu shortcut, or an
// installer's program directory, and a relative root fails both ways: it lands somewhere
// unwritable, so every save fails, or it makes the settings belong to that FOLDER rather than to
// the user, so moving the exe loses them. Both were seen on a Windows bench.
//
// Three sources, in order:
//   1. MM_DATA_DIR, for tests and for anyone who wants the data somewhere specific.
//   2. "build", when the working directory is a repo checkout. Keeps the dev loop, the gate
//      scripts, and a developer's existing .config exactly where they already are.
//   3. The OS's per-user application-data directory.

namespace {

// The OS convention for per-user application data. Empty when the environment names no home, which
// is a real case in a bare service account; the caller falls back rather than writing to "/".
std::filesystem::path userDataDir() {
#ifdef _WIN32
    // LOCALAPPDATA, not APPDATA: this is machine-local state and has no business roaming.
    if (const char* base = std::getenv("LOCALAPPDATA"); base && *base)
        return std::filesystem::path(base) / "projectMM";
#elif defined(__APPLE__)
    if (const char* home = std::getenv("HOME"); home && *home)
        return std::filesystem::path(home) / "Library" / "Application Support" / "projectMM";
#else
    if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg)
        return std::filesystem::path(xdg) / "projectMM";
    if (const char* home = std::getenv("HOME"); home && *home)
        return std::filesystem::path(home) / ".local" / "share" / "projectMM";
#endif
    return {};
}

// A checkout is recognized by CMakeLists.txt AND moondeck/ in the working directory. Both, because
// CMakeLists.txt alone is true in the root of every CMake project there is, and a developer whose
// shell happens to sit in an unrelated one would get projectMM's settings written into THAT
// project's build/, which is the per-folder loss this whole change removes.
//
// Deliberately the working directory and not the executable's location:
// `./build/windows/Release/projectMM` run from the repo root is the dev loop this preserves, and an
// installed copy is never launched that way. In a checkout the root is `build/fs` (config under
// `build/fs/.config`), a subdirectory rather than the build tree itself: see below.
std::filesystem::path defaultRoot() {
    if (const char* env = std::getenv("MM_DATA_DIR"); env && *env)
        return std::filesystem::path(env);
    std::error_code ec;
    if (std::filesystem::exists("CMakeLists.txt", ec) && !ec
        && std::filesystem::is_directory("moondeck", ec) && !ec)
        // `build/fs`, not `build`: the device's filesystem is what the File Manager shows as its
        // root, and rooting it at the build directory listed CMake caches, object archives and
        // every ESP32 variant's build folder beside the four directories a device actually has.
        // A subfolder makes the desktop look like a board, which is the point of the desktop
        // build: what a user sees there has to be what they will see on hardware.
        return std::filesystem::path("build") / "fs";
    std::filesystem::path user = userDataDir();
    return user.empty() ? std::filesystem::path("build") : user;
}

std::filesystem::path fsRoot_{defaultRoot()};

// Map "/.config/foo.json" → "<root>/.config/foo.json". Strips leading '/'s, normalizes
// the result, and rejects paths that escape fsRoot_ (e.g. "../../etc/passwd"). Returns
// an empty path on rejection; callers already treat empty/nonexistent as failure.
std::filesystem::path toFsPath(const char* path) {
    if (!path) return {};
    while (*path == '/') path++;  // strip any number of leading slashes
    std::filesystem::path candidate = (fsRoot_ / path).lexically_normal();
    std::filesystem::path rootNormal = fsRoot_.lexically_normal();
    // Prefix check on the normalized string: candidate must start with rootNormal followed
    // by either end-of-string or a separator. Iterator comparison is more robust against
    // trailing-slash quirks; mismatched_first signals an escape.
    auto [r, c] = std::mismatch(rootNormal.begin(), rootNormal.end(),
                                candidate.begin(), candidate.end());
    if (r != rootNormal.end()) return {};  // candidate diverges before consuming all of rootNormal
    return candidate;
}
}

void fsSetRoot(const char* path) {
    fsRoot_ = (path && *path) ? std::filesystem::path(path) : defaultRoot();
}

const char* fsRootPath() {
    // Refreshed per call rather than cached at set time, so it cannot go stale after fsSetRoot.
    // Diagnostics only, and the desktop build reports it from one thread.
    static std::string cached;
    cached = fsRoot_.string();
    return cached.c_str();
}

bool fsMount() {
    // Desktop has no volume to mount, but it DOES have a root that may not exist yet and may not
    // be writable. Establishing that here is what turns an unusable location into ONE line at
    // startup instead of one write failure per save, forever.
    std::error_code ec;
    std::filesystem::create_directories(fsRoot_, ec);
    if (!std::filesystem::is_directory(fsRoot_, ec)) return false;
    // Existence does not imply writability: a read-only extraction, a protected folder, or a
    // directory owned by another user all exist happily and reject the first write. create_
    // directories is silent about all three, so probe with the operation that actually matters.
    const auto probe = fsRoot_ / ".mm-write-probe";
    std::error_code rm;
    std::filesystem::remove(probe, rm);
    FILE* f = std::fopen(probe.string().c_str(), "wb");
    if (!f) return false;
    std::fclose(f);
    std::filesystem::remove(probe, rm);
    return true;
}

void fsUnmount() {}

bool fsMkdir(const char* path) {
    std::error_code ec;
    std::filesystem::create_directories(toFsPath(path), ec);
    return !ec;
}

bool fsExists(const char* path) {
    std::error_code ec;
    return std::filesystem::exists(toFsPath(path), ec);
}

bool fsRemove(const char* path) {
    std::error_code ec;
    return std::filesystem::remove(toFsPath(path), ec);
}

int fsRead(const char* path, char* buf, size_t maxLen) {
    if (!buf || maxLen == 0) return -1;
    // path::c_str() returns wchar_t* on Windows; std::fopen needs char*. Go via
    // .string() so the call compiles on both. Costs one std::string allocation
    // per read — acceptable for /.config/*.json reads (rare, small).
    FILE* f = std::fopen(toFsPath(path).string().c_str(), "rb");
    if (!f) return -1;
    size_t n = std::fread(buf, 1, maxLen - 1, f);
    std::fclose(f);
    buf[n] = 0;
    return static_cast<int>(n);
}

// Open a temp file for atomic-write, owner-only (0600) where the OS has file modes.
//
// std::fopen creates with 0666 & ~umask, so on a typical umask 022 the file lands world-readable
// — and these are /.config/*.json, which hold WiFi PSKs and MQTT passwords. Nothing here is
// multi-user on ESP32 (LittleFS has no modes at all, so the platform layer's ESP32 half is
// unaffected), but the desktop build runs on real machines with real other users.
//
// POSIX gets O_CREAT|O_EXCL with an explicit 0600 — EXCL because a pre-existing temp file is
// either a crashed run's leftover or someone else's, and inheriting its mode would defeat the
// point. Windows has no mode_t; its files inherit the parent directory's ACL, which is the
// platform's own answer to the same question, so it keeps plain fopen.
static FILE* openTempOwnerOnly(const char* path) {
#ifdef _WIN32
    return std::fopen(path, "wb");
#else
    ::unlink(path);                       // clear a leftover so O_EXCL cannot fail on our own temp
    const int fd = ::open(path, O_WRONLY | O_CREAT | O_EXCL | O_TRUNC, 0600);
    if (fd < 0) return nullptr;
    FILE* f = ::fdopen(fd, "wb");
    if (!f) ::close(fd);                  // fdopen failure leaves the descriptor ours to release
    return f;
#endif
}

bool fsWriteAtomic(const char* path, const char* data, size_t len) {
    auto target = toFsPath(path);
    auto tmp = target;
    tmp += ".tmp";

    FILE* f = openTempOwnerOnly(tmp.string().c_str());
    if (!f) return false;
    size_t written = std::fwrite(data, 1, len, f);
    if (written != len) {
        std::fclose(f);
        std::error_code ec;
        std::filesystem::remove(tmp, ec);
        return false;
    }
    std::fflush(f);
#ifdef _WIN32
    int fd = ::_fileno(f);
    if (fd >= 0) ::_commit(fd);  // Windows equivalent of fsync
#else
    int fd = ::fileno(f);
    if (fd >= 0) ::fsync(fd);
#endif
    std::fclose(f);

    std::error_code ec;
    std::filesystem::rename(tmp, target, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}

long fsSize(const char* path) {
    std::error_code ec;
    auto p = toFsPath(path);
    if (!std::filesystem::is_regular_file(p, ec)) return -1;
    const auto sz = std::filesystem::file_size(p, ec);
    return ec ? -1 : static_cast<long>(sz);
}

int fsReadAt(const char* path, long offset, char* buf, size_t len) {
    if (!buf) return -1;
    FILE* f = std::fopen(toFsPath(path).string().c_str(), "rb");
    if (!f) return -1;
    if (std::fseek(f, offset, SEEK_SET) != 0) { std::fclose(f); return -1; }
    const size_t n = std::fread(buf, 1, len, f);
    std::fclose(f);
    return static_cast<int>(n);   // 0 at EOF
}

bool fsWriteStream(const char* path, FsWriteSrc src, void* user) {
    if (!src) return false;
    auto target = toFsPath(path);
    auto tmp = target;
    tmp += ".tmp";

    FILE* f = openTempOwnerOnly(tmp.string().c_str());
    if (!f) return false;
    // Pull chunks from the source and write each straight through — fixed buffer, any file size.
    // `abort` set by the source (a short/timed-out upload) means the data is incomplete → discard.
    char chunk[1024];
    bool ok = true, abort = false;
    for (;;) {
        const size_t got = src(chunk, sizeof(chunk), user, &abort);
        if (abort) { ok = false; break; }
        if (got == 0) break;                                    // clean end of stream
        if (std::fwrite(chunk, 1, got, f) != got) { ok = false; break; }
    }
    std::fflush(f);
#ifdef _WIN32
    int fd = ::_fileno(f);
    if (fd >= 0) ::_commit(fd);
#else
    int fd = ::fileno(f);
    if (fd >= 0) ::fsync(fd);
#endif
    std::fclose(f);

    std::error_code ec;
    if (!ok) { std::filesystem::remove(tmp, ec); return false; }
    std::filesystem::rename(tmp, target, ec);
    if (ec) { std::filesystem::remove(tmp, ec); return false; }
    return true;
}

void fsList(const char* dir, FsListCb cb, void* user) {
    if (!cb) return;
    std::error_code ec;
    auto p = toFsPath(dir);
    if (!std::filesystem::exists(p, ec)) return;
    for (auto& entry : std::filesystem::directory_iterator(p, ec)) {
        if (ec) break;
        // path::filename().c_str() returns wchar_t* on Windows; the callback
        // wants char*. Round-trip through .string() to get a portable view.
        std::string name = entry.path().filename().string();
        const bool isDir = entry.is_directory(ec);
        std::error_code sizeEc;
        const auto sz = isDir ? 0u : static_cast<uint32_t>(std::filesystem::file_size(entry.path(), sizeEc));
        cb(name.c_str(), isDir, sizeEc ? 0u : sz, user);
    }
}

size_t filesystemUsed() {
    // Sum of file sizes under ./.config/
    std::error_code ec;
    auto root = toFsPath("/.config");
    if (!std::filesystem::exists(root, ec)) return 0;
    size_t total = 0;
    for (auto& entry : std::filesystem::recursive_directory_iterator(root, ec)) {
        if (ec) break;
        if (entry.is_regular_file(ec)) {
            total += entry.file_size(ec);
        }
    }
    return total;
}

size_t filesystemTotal() {
    // Desktop has no fixed quota; report a notional 384 KB to match the 4MB ESP32 partition.
    return 384 * 1024;
}

// Network stubs (desktop has no WiFi/Ethernet hardware)

void setEthConfig(const EthPinConfig&) {}   // no eth on desktop; ethInit stubs false
void ethStop() {}                           // no eth on desktop
bool ethInit() { return false; }

// Raw-frame capture: the desktop half of the ethSendRaw seam. Sending a real L2 frame from a host
// process needs a raw socket and root, which no test should ask for — so the host RECORDS what the
// driver emitted instead. That is what lets PanelCardDriver and its tests build and run everywhere
// (the desktop-runs-everything rule), with the packet bytes pinned on the host and only the wire
// itself left to the bench.
//
// Fixed capacity, no allocation: a test asserts over the first few frames of a render tick, and an
// unbounded recorder would turn a long run into unbounded memory. Frames past the cap are counted
// but not stored, so an overrun shows up as a count the test can see.
namespace {
// Sized for the largest frame sequence a test asserts over: a 128-row wall is 2 brightness + 128
// rows + 2 sync = 132.
//
// Allocated on FIRST CAPTURE, not from boot: this is a test seam, and as a plain static array it
// cost ~195 KB of BSS in the shipped desktop/Pi binary — a deployment that binds a real interface
// never records a frame and would have paid for it anyway. Same lazy-allocation reasoning as the
// task-snapshot scratch in the ESP32 platform layer. Never freed: freeing would put the allocation
// back on a path that runs per frame, and one buffer per process is the point.
constexpr size_t kEthTestMaxFrames = 132;
uint8_t (*ethTestFrames_)[kEthTestFrameMax] = nullptr;
size_t  ethTestLens_[kEthTestMaxFrames] = {};
size_t  ethTestCount_ = 0;
bool    ethTestSendFails_ = false;
uint16_t ethTestLinkSpeed_ = 1000;   // desktop reports gigabit unless a test says otherwise
int      ethRawClaims_ = 0;          // drivers holding the link for direct L2 use
uint32_t ethSendFails_ = 0;          // consecutive ethSendRaw failures (the streak)
uint32_t ethFailTotal_ = 0;          // cumulative since boot; what ethSendFailCounts reports
uint32_t ethRestarts_ = 0;           // ethRestartTx() calls, for the once-per-wedge test
bool     ethRestartFails_ = false;   // simulated recovery failure
// The bound raw socket, or -1 for capture mode (the default, and all any test sees).
int      ethRawFd_ = -1;
unsigned ethRawIfIndex_ = 0;         // Linux AF_PACKET needs the index; BPF binds by name

#ifdef _WIN32
// --- Npcap/WinPcap, loaded at RUN TIME ---------------------------------------------------------
//
// Windows has no kernel path for sending a raw L2 frame: it takes a third-party driver, which is
// what Npcap is and what ColorLight's own LEDVision uses. wpcap.dll is resolved with LoadLibrary
// rather than linked, and that is deliberate rather than stylistic. mm_platform is a PUBLIC
// dependency of mm_core, projectMM, mm_tests and mm_scenarios, so linking wpcap would make the
// Npcap SDK a build requirement for CI and for every contributor, to compile a path most of them
// never run. Loading on demand means the binary builds and runs identically without Npcap, and
// reports that raw send is unavailable instead of failing to link.
//
// The whole surface is five functions, declared here with pcap's own signatures rather than by
// including pcap.h, which would reintroduce the SDK dependency this exists to avoid.
struct PcapIf { PcapIf* next; char* name; char* description; /* remaining fields unused */ };
using PcapT = struct pcap;
// pcap's send queue: a preformatted block of (header, bytes) pairs the kernel transmits in one
// call. Layout must match wpcap's exactly — it is written by pcap_sendqueue_queue and read by
// pcap_sendqueue_transmit, so these are ABI, not convenience.
struct PcapSendQueue { unsigned maxlen; unsigned len; char* buffer; };
struct PcapTimeval { long tv_sec; long tv_usec; };            // Windows long is 32-bit
struct PcapPktHdr  { PcapTimeval ts; unsigned caplen; unsigned len; };
using PcapOpenLiveFn    = PcapT* (*)(const char*, int, int, int, char*);
using PcapSendPacketFn  = int (*)(PcapT*, const unsigned char*, int);
using PcapCloseFn       = void (*)(PcapT*);
using PcapFindAllDevsFn = int (*)(PcapIf**, char*);
using PcapFreeAllDevsFn = void (*)(PcapIf*);
using PcapQAllocFn      = PcapSendQueue* (*)(unsigned);
using PcapQQueueFn      = int (*)(PcapSendQueue*, const PcapPktHdr*, const unsigned char*);
using PcapQTransmitFn   = unsigned (*)(PcapT*, PcapSendQueue*, int);
using PcapQDestroyFn    = void (*)(PcapSendQueue*);

HMODULE           wpcapLib_ = nullptr;
PcapOpenLiveFn    pcapOpenLive_ = nullptr;
PcapSendPacketFn  pcapSendPacket_ = nullptr;
PcapCloseFn       pcapClose_ = nullptr;
PcapFindAllDevsFn pcapFindAllDevs_ = nullptr;
PcapFreeAllDevsFn pcapFreeAllDevs_ = nullptr;
PcapQAllocFn      pcapQAlloc_ = nullptr;
PcapQQueueFn      pcapQQueue_ = nullptr;
PcapQTransmitFn   pcapQTransmit_ = nullptr;
PcapQDestroyFn    pcapQDestroy_ = nullptr;
PcapT*            pcapHandle_ = nullptr;   // the open adapter, or null for capture mode
// The batch ethSendRaw fills and ethFlushRaw hands to the kernel. Allocated once at BIND time, not
// per frame: ethSendRaw is MM_NONBLOCKING and must not allocate. Null when wpcap is too old to
// offer the queue API, in which case sends fall back to one syscall per packet.
PcapSendQueue*    pcapQueue_ = nullptr;
// Sized for one wall frame with headroom: the widest supported wall is 256 rows, plus brightness
// and sync frames, at the Ethernet maximum. ~400 KB of one-time allocation on a machine that has
// just chosen to drive an LED wall.
constexpr unsigned kSendQueueBytes = 264u * (unsigned)(kEthTestFrameMax + sizeof(PcapPktHdr));
// The adapter GUID the handle belongs to, so ethLinkUp/ethLinkSpeedMbps report THAT NIC. The GUID
// rather than the description, because the description is not always THERE: pcap reports none at
// all for some adapters (a USB NIC on this bench reports none), while `\Device\NPF_{GUID}` is the
// one identifier every Windows pcap device carries. See winAdapterLink.
char              boundGuid_[40] = {};   // "{8BB7C86E-E3D1-4842-8333-DAD18FD0ADD5}" + NUL

/// Resolve wpcap.dll once. False when Npcap is not installed, which is an ordinary state.
bool wpcapLoad() {
    if (pcapSendPacket_) return true;
    if (!wpcapLib_) wpcapLib_ = ::LoadLibraryA("wpcap.dll");
    if (!wpcapLib_) return false;
    auto sym = [](HMODULE m, const char* n) {
        return reinterpret_cast<void*>(::GetProcAddress(m, n));
    };
    pcapOpenLive_    = reinterpret_cast<PcapOpenLiveFn>(sym(wpcapLib_, "pcap_open_live"));
    pcapSendPacket_  = reinterpret_cast<PcapSendPacketFn>(sym(wpcapLib_, "pcap_sendpacket"));
    pcapClose_       = reinterpret_cast<PcapCloseFn>(sym(wpcapLib_, "pcap_close"));
    pcapFindAllDevs_ = reinterpret_cast<PcapFindAllDevsFn>(sym(wpcapLib_, "pcap_findalldevs"));
    pcapFreeAllDevs_ = reinterpret_cast<PcapFreeAllDevsFn>(sym(wpcapLib_, "pcap_freealldevs"));
    // The batch API is OPTIONAL: it is a WinPcap/Npcap extension, absent from some builds. When it
    // is missing the sends below stay one-syscall-per-packet, which works and merely jitters.
    pcapQAlloc_    = reinterpret_cast<PcapQAllocFn>(sym(wpcapLib_, "pcap_sendqueue_alloc"));
    pcapQQueue_    = reinterpret_cast<PcapQQueueFn>(sym(wpcapLib_, "pcap_sendqueue_queue"));
    pcapQTransmit_ = reinterpret_cast<PcapQTransmitFn>(sym(wpcapLib_, "pcap_sendqueue_transmit"));
    pcapQDestroy_  = reinterpret_cast<PcapQDestroyFn>(sym(wpcapLib_, "pcap_sendqueue_destroy"));
    return pcapOpenLive_ && pcapSendPacket_ && pcapClose_ && pcapFindAllDevs_ && pcapFreeAllDevs_;
}

/// Case-insensitive substring test, so an adapter can be named the way Windows shows it.
bool containsNoCase(const char* haystack, const char* needle) {
    if (!haystack || !needle || !*needle) return false;
    for (const char* h = haystack; *h; h++) {
        const char* a = h;
        const char* b = needle;
        while (*a && *b && std::tolower(static_cast<unsigned char>(*a)) ==
                           std::tolower(static_cast<unsigned char>(*b))) { a++; b++; }
        if (!*b) return true;
    }
    return false;
}

/// Case-insensitive equality, for two strings already in the same canonical form.
bool equalsNoCase(const char* a, const char* b) {
    for (; *a && *b; a++, b++) {
        if (std::tolower(static_cast<unsigned char>(*a)) !=
            std::tolower(static_cast<unsigned char>(*b))) return false;
    }
    return !*a && !*b;
}

/// The `{GUID}` out of a pcap device name (`\Device\NPF_{8BB7C86E-...}`), braces included.
bool guidFromPcapName(const char* name, char* out, size_t cap) {
    if (!name || !out || cap == 0) return false;
    const char* open = std::strchr(name, '{');
    if (!open) return false;
    const char* close = std::strchr(open, '}');
    if (!close) return false;
    const size_t n = static_cast<size_t>(close - open) + 1;
    if (n >= cap) return false;
    std::memcpy(out, open, n);
    out[n] = '\0';
    return true;
}

/// A MIB row's GUID in the spelling pcap uses, so the two can be compared as text.
void guidToString(const GUID& g, char* out, size_t cap) {
    std::snprintf(out, cap, "{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
                  static_cast<unsigned long>(g.Data1), g.Data2, g.Data3,
                  g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3],
                  g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
}

/// The description WINDOWS shows for a pcap device, found through the interface table by GUID,
/// with the adapter's LINK SPEED appended when Windows states one ("Realtek PCIe GbE, 1 Gb").
/// This exists because pcap's own description can be absent: without it such an adapter is
/// unnameable, since the only text left to match is a 49-character device path.
///
/// The speed rides in the label because the name alone does not say what a picker needs to know:
/// a panel wall wants the 1 Gb NIC, and a list of plausible-looking names hides which entries are
/// a 2.5 Gb USB dongle, a Wi-Fi radio, or a Hyper-V virtual switch. Windows reports 0 or ~0 for
/// an adapter whose speed it will not state (typically one that is down), and those get no suffix
/// rather than a fabricated "0 Mb".
/// The interface-table row behind a pcap device, matched on the GUID in its device name. One home
/// for the walk, because the label and the is-this-a-real-NIC test both need the same row.
const MIB_IF_ROW2* winRowForPcapName(const MIB_IF_TABLE2* table, const char* pcapName) {
    if (!table) return nullptr;
    char want[40];
    if (!guidFromPcapName(pcapName, want, sizeof(want))) return nullptr;
    for (ULONG i = 0; i < table->NumEntries; i++) {
        char have[40];
        guidToString(table->Table[i].InterfaceGuid, have, sizeof(have));
        if (equalsNoCase(have, want)) return &table->Table[i];
    }
    return nullptr;
}

/// Can this adapter carry panel frames? Only physical Ethernet can.
///
/// IF_TYPE_ETHERNET_CSMACD on its own is not the test: measured on a Windows bench, the Hyper-V
/// vSwitch ports, every WAN miniport, the network bridge and Bluetooth PAN all report that type
/// too. HardwareInterface is what separates them from a NIC with a socket on it. Wi-Fi fails the
/// type test instead (IF_TYPE_IEEE80211), which is the right answer for a card that needs a wire.
bool winIsPanelCapableNic(const MIB_IF_ROW2* row) {
    return row && row->Type == IF_TYPE_ETHERNET_CSMACD
        && row->InterfaceAndOperStatusFlags.HardwareInterface;
}

bool winDescForPcapName(const MIB_IF_TABLE2* table, const char* pcapName, char* out, size_t cap) {
    if (!out || cap == 0) return false;
    const MIB_IF_ROW2* row = winRowForPcapName(table, pcapName);
    if (!row) return false;
    // Narrow the wide description as we copy: an adapter description is ASCII.
    size_t n = 0;
    for (; n + 1 < cap && row->Description[n]; n++) {
        out[n] = static_cast<char>(row->Description[n]);
    }
    out[n] = '\0';
    if (n == 0) return false;

    // Same source and the same unknown-speed guard as ethLinkSpeedMbps (winAdapterLink);
    // converted to Mbit here so the shared formatter takes one unit from every OS.
    const unsigned long long bps = row->TransmitLinkSpeed;
    if (bps == 0 || bps == ~0ULL) return true;
    appendLinkSpeed(out, cap, static_cast<unsigned>(bps / 1000000ULL));
    return true;
}
#endif  // _WIN32
}  // namespace

// Open a raw L2 socket on `ifName` so a host build drives panels for real — the deployment a Pi or
// a mini-PC covers, and the same code path the ESP32 takes. Linux uses AF_PACKET, macOS BPF; both
// need root (or CAP_NET_RAW), so an ordinary test run simply stays in capture mode.
bool ethBindRawInterface(const char* ifName) {
#ifdef _WIN32
    // Close any previous handle first: `interface` is a live control, so a rebind must not leak
    // the old adapter (CLAUDE.md, every setting applies live).
    if (pcapHandle_ && pcapClose_) { pcapClose_(pcapHandle_); }
    pcapHandle_ = nullptr;
    if (pcapQueue_ && pcapQDestroy_) { pcapQDestroy_(pcapQueue_); }
    pcapQueue_ = nullptr;
    boundGuid_[0] = '\0';
    if (!ifName || !ifName[0]) return true;   // explicit return to capture mode, as on POSIX
    if (!wpcapLoad()) return false;           // no Npcap installed: the driver reports it

    // Match the user's string against pcap's device name, pcap's description, and the description
    // WINDOWS shows for the same adapter — case-insensitively, first hit wins. A pcap device is
    // `\Device\NPF_{GUID}`, 49 characters against a 16-byte control, so a substring of a
    // description is the only spelling that fits. The Windows lookup is not a nicety: pcap reports
    // NO description for some adapters, and for those nothing a user could type would match at all.
    // An exact device name still matches, which keeps the control meaning the same thing it means
    // on Linux and macOS: name the interface.
    PcapIf* devs = nullptr;
    char err[256] = {};
    if (pcapFindAllDevs_(&devs, err) != 0 || !devs) return false;
    MIB_IF_TABLE2* table = nullptr;
    if (::GetIfTable2(&table) != NO_ERROR) table = nullptr;   // fall back to pcap's own text
    const PcapIf* hit = nullptr;
    for (const PcapIf* d = devs; d; d = d->next) {
        if (containsNoCase(d->name, ifName) || containsNoCase(d->description, ifName)) { hit = d; break; }
        char desc[256];
        if (winDescForPcapName(table, d->name, desc, sizeof(desc)) &&
            containsNoCase(desc, ifName)) { hit = d; break; }
    }
    if (table) ::FreeMibTable(table);
    if (!hit) { pcapFreeAllDevs_(devs); return false; }

    // snaplen 65536, non-promiscuous, 1 ms read timeout. This handle only ever sends; promiscuous
    // capture would cost interrupts for frames nothing reads.
    PcapT* h = pcapOpenLive_(hit->name, 65536, 0, 1, err);
    if (h) {
        // Keep the GUID, which is what the link-state query matches on. The description was the
        // old key, and it fell back to the DEVICE NAME when pcap reported none — a string no MIB
        // row can ever match, so a perfectly bound adapter reported "no ethernet link" forever.
        guidFromPcapName(hit->name, boundGuid_, sizeof(boundGuid_));
    }
    pcapFreeAllDevs_(devs);
    pcapHandle_ = h;
    // Allocate the batch here, off the hot path, so ethSendRaw only ever fills it.
    if (h && pcapQAlloc_ && pcapQQueue_ && pcapQTransmit_) pcapQueue_ = pcapQAlloc_(kSendQueueBytes);
    return h != nullptr;
#else
    if (ethRawFd_ >= 0) { ::close(ethRawFd_); ethRawFd_ = -1; }
    ethRawIfIndex_ = 0;
    if (!ifName || !ifName[0]) return true;   // explicit return to capture mode

#if defined(__linux__)
    const int fd = ::socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (fd < 0) return false;
    const unsigned idx = if_nametoindex(ifName);
    if (idx == 0) { ::close(fd); return false; }
    ethRawFd_ = fd;
    ethRawIfIndex_ = idx;
    return true;
#elif defined(__APPLE__)
    // BPF has no single device: open the first free /dev/bpfN, then bind it to the interface.
    for (int i = 0; i < 99; i++) {
        char dev[24];
        std::snprintf(dev, sizeof(dev), "/dev/bpf%d", i);
        const int fd = ::open(dev, O_RDWR);
        if (fd < 0) continue;              // busy or no permission — try the next
        ifreq ifr = {};
        std::snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", ifName);
        if (::ioctl(fd, BIOCSETIF, &ifr) < 0) { ::close(fd); return false; }
        // Write whole frames as given. Without this BPF supplies its OWN source MAC, overwriting
        // the fixed one the cards filter on — the frames would go out well-formed and be ignored,
        // which is the hardest kind of failure to diagnose. So a failure here fails the bind.
        unsigned hdrComplete = 1;
        if (::ioctl(fd, BIOCSHDRCMPLT, &hdrComplete) < 0) { ::close(fd); return false; }
        ethRawFd_ = fd;
        return true;
    }
    return false;
#else
    (void)ifName;
    return false;   // no raw-L2 path on this host OS
#endif
#endif  // _WIN32
}

// Send the frame on the bound interface, or record it when none is bound. The capture branch is
// what every unit test exercises; the raw branch is what makes a host a panel controller.
bool ethSendRaw(const uint8_t* frame, size_t len) MM_NONBLOCKING {
    if (!frame || len == 0) return false;
    if (ethTestSendFails_) { ethSendFails_++; ethFailTotal_++; return false; }   // simulated link-down / full ring

#ifdef _WIN32
    // Bound adapter: send for real. Unbound (the default, and every unit test) falls through to the
    // capture ring below, so the Windows path gains sending without changing what tests observe.
    //
    // BATCHED when the queue API is available. A wall frame is ~131 packets that must all land
    // inside the card's inter-frame window, and one pcap_sendpacket per packet is one kernel
    // transition per packet: measured at 0.9-5.6 ms per frame on a 128x127 wall, a 5x spread that
    // the cards show as stutter because they latch on the sync frame and have no buffering.
    // Queuing costs a memcpy and hands the whole burst over in a single call from ethFlushRaw.
    if (pcapHandle_ && pcapQueue_ && pcapQQueue_) {
        PcapPktHdr hdr = {};
        hdr.caplen = static_cast<unsigned>(len);
        hdr.len    = static_cast<unsigned>(len);
        // NOTE the streak is not cleared here: queuing a packet into a buffer says nothing about
        // whether it reached the wire. Only ethFlushRaw, where pcap_sendqueue_transmit reports how
        // many bytes actually went out, is in a position to say the link is working. Clearing it on
        // enqueue would keep ethSendFailStreak() at zero forever, and that streak is what the driver
        // watches to detect a wedged link and call ethRestartTx.
        if (pcapQQueue_(pcapQueue_, &hdr, frame) == 0) return true;
        // Queue full: flush what we have and retry once, so an unexpectedly large wall degrades to
        // two batches rather than dropping the rest of the frame.
        ethFlushRaw();
        if (pcapQQueue_(pcapQueue_, &hdr, frame) == 0) return true;
        ethSendFails_++; ethFailTotal_++;
        return false;
    }
    if (pcapHandle_ && pcapSendPacket_) {
        const int rc = pcapSendPacket_(pcapHandle_, frame, static_cast<int>(len));
        if (rc != 0) { ethSendFails_++; ethFailTotal_++; return false; }
        ethSendFails_ = 0;
        return true;
    }
#endif

#ifndef _WIN32
    if (ethRawFd_ >= 0) {
#if defined(__linux__)
        sockaddr_ll dst = {};
        dst.sll_family = AF_PACKET;
        dst.sll_ifindex = static_cast<int>(ethRawIfIndex_);
        dst.sll_halen = 6;
        std::memcpy(dst.sll_addr, frame, 6);   // destination MAC is the frame's first 6 bytes
        const ssize_t n = ::sendto(ethRawFd_, frame, len, 0,
                                   reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
#else
        const ssize_t n = ::write(ethRawFd_, frame, len);
#endif
        // Track failures on the REAL send path too, not just the capture path: a bound host is
        // where frames actually reach a wire, so a streak here is the one that matters.
        if (n != static_cast<ssize_t>(len)) { ethSendFails_++; ethFailTotal_++; return false; }
        ethSendFails_ = 0;
        return true;
    }
#endif

    if (ethTestCount_ < kEthTestMaxFrames) {
        if (!ethTestFrames_) {
            ethTestFrames_ = static_cast<uint8_t(*)[kEthTestFrameMax]>(
                std::calloc(kEthTestMaxFrames, kEthTestFrameMax));
        }
        if (ethTestFrames_) {
            // Record the TRUE length even when the copy is clipped, so an oversized frame is visible
            // as a length no reader expected rather than as silently short data.
            const size_t copy = len < kEthTestFrameMax ? len : kEthTestFrameMax;
            std::memcpy(ethTestFrames_[ethTestCount_], frame, copy);
            ethTestLens_[ethTestCount_] = len;
        }
    }
    ethTestCount_++;
    ethSendFails_ = 0;
    return true;
}

uint32_t ethSendFailStreak() MM_NONBLOCKING { return ethSendFails_; }

// A host socket has no driver link state to refuse against, so every failure is the
// ring-full analogue (a full socket buffer).
void ethSendFailCounts(uint32_t& linkDown, uint32_t& ringFull) MM_NONBLOCKING {
    linkDown = 0; ringFull = ethFailTotal_;
}

// A host raw socket has no driver-internal link state to desync, so there is nothing to
// restart, so clear the streak and let a test exercise the driver's recovery path.
bool ethRestartTx() {
    ethRestarts_++;
    if (ethRestartFails_) return false;
    ethSendFails_ = 0;
    return true;
}

void setTestEthRestartFails(bool fail) { ethRestartFails_ = fail; }

uint32_t ethRestartCountForTest() { return ethRestarts_; }

// See platform.h: a claim stated by the driver, reference-counted.
// Hand the batched burst to the kernel. See the header for why this seam exists.
//
// A no-op everywhere except a Windows host with the pcap queue API: Linux and macOS already give
// the frame to the kernel inside ethSendRaw, so there is nothing held back to flush.
void ethFlushRaw() MM_NONBLOCKING {
#ifdef _WIN32
    if (!pcapHandle_ || !pcapQueue_ || !pcapQTransmit_ || pcapQueue_->len == 0) return;
    // sync=0: transmit at wire speed rather than replaying the queued timestamps. The card wants
    // the whole burst inside its inter-frame window, which is the opposite of paced playback.
    const unsigned queued = pcapQueue_->len;
    const unsigned sent = pcapQTransmit_(pcapHandle_, pcapQueue_, 0);
    // The one place that knows the burst actually left, so it owns BOTH ends of the streak: a short
    // write is the failure ethSendFailStreak counts, and a complete one is the only honest reason to
    // clear it.
    if (sent < queued) { ethSendFails_++; ethFailTotal_++; }
    else               { ethSendFails_ = 0; }
    // Reset for the next frame: the queue is a buffer, and transmit does not rewind it.
    pcapQueue_->len = 0;
#endif
}

void ethClaimRawL2(bool claim) {
    if (claim) ethRawClaims_++;
    else if (ethRawClaims_ > 0) ethRawClaims_--;
}

bool ethRawL2Claimed() MM_NONBLOCKING { return ethRawClaims_ > 0; }

// The host has no negotiated link. Report gigabit so the driver's speed check passes on desktop and
// its tests exercise the send path rather than the too-slow branch (which has its own test via
// setTestEthLinkSpeed).
// Link state and negotiated speed.
//
// On Windows these describe the adapter ethBindRawInterface opened, queried through IPHLPAPI. It
// matters here rather than being a nicety: PanelCardDriver warns below 1000 Mbit because a
// ColorLight card has no buffering and no flow control, so a 100 Mbit link tears the panel while
// every frame still "sends" successfully. A hardcoded 1000 would make that warning inert, which is
// worse than absent — it would state a fact nobody measured.
//
// Elsewhere on the desktop there is no Ethernet peripheral to describe, so these keep the stub
// values and ethTestLinkSpeed_ lets a test choose what the driver sees.
#ifdef _WIN32
namespace {
/// (linkUp, mbps) for the adapter ethBindRawInterface opened.
///
/// Matched on the adapter GUID through GetIfTable2 — NOT through GetAdaptersAddresses, because a
/// NIC bound to a Hyper-V external vSwitch does not appear there at all: Windows reports the
/// virtual adapter and hides the physical one the switch owns. Measured here, where pcap opens
/// `\Device\NPF_{7DD559D5-...}` (the Realtek) and that GUID is in no GetAdaptersAddresses row.
/// GetIfTable2 lists the physical interface AND carries the same GUID pcap put in the device name,
/// so one exact key covers both a virtualized NIC and an adapter pcap describes as nothing at all.
/// The description cannot do that: absent on some adapters, filter-suffixed on others.
bool winAdapterLink(uint16_t& mbps) {
    mbps = 0;
    if (!boundGuid_[0]) return false;
    MIB_IF_TABLE2* table = nullptr;
    if (::GetIfTable2(&table) != NO_ERROR || !table) return false;
    bool up = false;
    for (ULONG i = 0; i < table->NumEntries; i++) {
        const MIB_IF_ROW2& row = table->Table[i];
        char have[40];
        guidToString(row.InterfaceGuid, have, sizeof(have));
        if (!equalsNoCase(have, boundGuid_)) continue;
        up = (row.OperStatus == IfOperStatusUp);
        const unsigned long long bps = row.TransmitLinkSpeed;
        mbps = (bps == 0 || bps == ~0ULL) ? 0
             : static_cast<uint16_t>((bps / 1000000ULL) > 65535ULL ? 65535ULL : (bps / 1000000ULL));
        break;
    }
    ::FreeMibTable(table);
    return up;
}
}  // namespace

bool ethLinkUp() MM_NONBLOCKING { uint16_t m = 0; return winAdapterLink(m); }
bool ethConnected() MM_NONBLOCKING { return ethLinkUp(); }
uint16_t ethLinkSpeedMbps() MM_NONBLOCKING {
    uint16_t m = 0;
    if (winAdapterLink(m) && m) return m;
    return ethTestLinkSpeed_;   // unbound, or a speed Windows would not state
}
#else
bool ethLinkUp() MM_NONBLOCKING { return false; }
bool ethConnected() MM_NONBLOCKING { return false; }
uint16_t ethLinkSpeedMbps() MM_NONBLOCKING { return ethTestLinkSpeed_; }
#endif

size_t ethTestFrameCount() { return ethTestCount_; }
size_t ethTestFrameLength(size_t i) { return i < kEthTestMaxFrames ? ethTestLens_[i] : 0; }
const uint8_t* ethTestFrameData(size_t i) {
    return (ethTestFrames_ && i < kEthTestMaxFrames) ? ethTestFrames_[i] : nullptr;
}
void ethTestClearFrames() { ethTestCount_ = 0; ethSendFails_ = 0; ethFailTotal_ = 0; }
void setTestEthSendFails(bool fail) { ethTestSendFails_ = fail; }
void setTestEthLinkSpeed(uint16_t mbps) { ethTestLinkSpeed_ = mbps; }
void ethGetIPv4(uint8_t out[4]) MM_NONBLOCKING {
    // Desktop has no real interface state, but DevicesModule needs the host's LAN
    // IP to scan from (otherwise a desktop projectMM instance reports "no network" and
    // never sweeps). hostIp() resolves it via the outbound-route trick; report it
    // as the "ethernet" IP so DevicesModule's localIp() (eth-first) picks it up.
    out[0] = out[1] = out[2] = out[3] = 0;
    const char* ip = hostIp();
    if (ip && ip[0]) {
        // Parse the dotted-quad to octets with inet_pton (already used in this file)
        // — the platform layer doesn't include core/Control.h's parseDottedQuad.
        in_addr a{};
        if (inet_pton(AF_INET, ip, &a) == 1) {
            uint32_t n = a.s_addr;   // network byte order: octet 0 is the low byte
            out[0] = static_cast<uint8_t>(n & 0xff);
            out[1] = static_cast<uint8_t>((n >> 8) & 0xff);
            out[2] = static_cast<uint8_t>((n >> 16) & 0xff);
            out[3] = static_cast<uint8_t>((n >> 24) & 0xff);
        }
    }
}

// Test seam: the host has no STA radio, so wifiStaInit() reports "no STA" — unless a test fakes
// one to drive NetworkModule's WaitingSta path. Cross-thread atomic, the setTestNowMs contract.
static std::atomic<bool> testWifiStaAvailable{false};
void setTestWifiStaAvailable(bool available) { testWifiStaAvailable.store(available, std::memory_order_relaxed); }
bool wifiStaInit(const char* /*ssid*/, const char* /*password*/) {
    return testWifiStaAvailable.load(std::memory_order_relaxed);
}
bool wifiStaConnected() MM_NONBLOCKING { return false; }
void wifiStaGetIPv4(uint8_t out[4]) { out[0] = out[1] = out[2] = out[3] = 0; }
// Addressing is OS-managed on desktop; the static/DHCP setters are inert (no netif to reconfigure).
// The per-interface apply counter is the observable a host test pins the static-addressing path on.
static std::atomic<uint32_t> testStaticApplies[2] = {};   // indexed by NetIface
void netSetStaticIPv4(NetIface iface, const uint8_t[4], const uint8_t[4],
                      const uint8_t[4], const uint8_t[4]) {
    testStaticApplies[static_cast<uint8_t>(iface)].fetch_add(1, std::memory_order_relaxed);
}
uint32_t testNetStaticApplyCount(NetIface iface) {
    return testStaticApplies[static_cast<uint8_t>(iface)].load(std::memory_order_relaxed);
}
void netSetDhcp(NetIface /*iface*/) {}
void setHostname(const char* /*name*/) {}   // no DHCP client on desktop
void wifiStaStop() {}
int wifiStaRssi() { return 0; }
void wifiStaBssid(uint8_t out[6]) { std::memset(out, 0, 6); }
int wifiStaChannel() { return 0; }

bool wifiApInit(const char* /*apName*/, const char* /*ip*/) { return false; }
bool wifiApConnected() { return false; }
void wifiApStop() {}
uint32_t wifiApClientCount() { return 0; }

// Host sockets work regardless of the (stubbed) link predicates above, and there is
// no lwip-style init race — always socket-safe.
bool networkReady() { return true; }
int wifiTxPower() { return 0; }
// Match the API contract: 0 is a successful no-op (matches ESP-IDF
// MM_NO_WIFI stub semantics). Any non-zero value returns false since
// there's no radio to set on the desktop. The 0-as-success branch
// matters because NetworkModule's syncTxPower passes the ESP-IDF
// "no override" sentinel (80 quarter-dBm → full power, which maps to
// txPowerSetting_==0 in user-facing dBm) through this setter to lift
// any prior cap; on desktop the radio doesn't exist so "the cap is
// lifted" is trivially true.
bool wifiSetTxPower(int8_t quarterDbm) { return quarterDbm == 0; }

bool mdnsInit(const char* /*deviceName*/) { return false; }
void mdnsStop() {}
void mdnsShutdown() {}
// mDNS advertise is a device-only concern, so these are host stubs. Discovery is UDP
// presence (DevicesModule + WledPacket) over UdpSocket, which runs on desktop too — so the
// discovery path is unit-testable on the host with real loopback datagrams (a bound socket
// or DevicesModule::injectPacketForTest).

// OTA — no-op on desktop (no OTA partition). The /api/firmware/url route
// guards with `if constexpr (mm::platform::hasOta)` and returns 501 here,
// so this stub exists for compile coverage only.
bool http_fetch_to_ota(const char* /*url*/,
                       char* statusBuf, size_t statusBufLen,
                       uint32_t* bytesReadOut, uint32_t* bytesTotalOut) {
    if (statusBuf && statusBufLen > 0) {
        std::snprintf(statusBuf, statusBufLen, "unsupported on desktop");
    }
    if (bytesReadOut) *bytesReadOut = 0;
    if (bytesTotalOut) *bytesTotalOut = 0;
    return false;
}

bool otaWriteStream(FsWriteSrc /*src*/, void* /*user*/, size_t /*contentLen*/,
                    char* statusBuf, size_t statusBufLen, uint32_t* bytesReadOut) {
    // No OTA partition on desktop — call sites guard with `if constexpr (mm::platform::hasOta)`.
    if (statusBuf && statusBufLen > 0) std::snprintf(statusBuf, statusBufLen, "unsupported on desktop");
    if (bytesReadOut) *bytesReadOut = 0;
    return false;
}

// No partitions on desktop: there is no recovery image and nothing to boot into.
bool otaHasMoonBase() { return false; }
bool otaBootMoonBase() { return false; }
bool otaRunningMoonBase() { return false; }
// No factory partition off-device, so nothing to read a version from.
bool otaMoonBaseVersion(char*, size_t) { return false; }
bool otaMoonBaseBuild(char*, size_t) { return false; }
bool otaMoonBaseSize(uint32_t*, uint32_t*) { return false; }
// No factory partition to install into off-device.
bool otaFetchMoonBaseUrl(const char*, char* statusBuf, size_t statusBufLen,
                         uint32_t* bytesReadOut, uint32_t* bytesTotalOut) {
    if (statusBuf && statusBufLen > 0) std::snprintf(statusBuf, statusBufLen, "unsupported on desktop");
    if (bytesReadOut) *bytesReadOut = 0;
    if (bytesTotalOut) *bytesTotalOut = 0;
    return false;
}

// Desktop has no factory partition, so this cannot install anything. It DOES run the vetting,
// which is the part worth exercising off-device: the checks below are what stand between a
// mistyped URL and a board with no recovery image, and they are pure byte inspection. Tests
// drive this to prove each rejection fires; the write itself has no meaning here and the
// function reports so, which also keeps a desktop caller from believing it worked.
// Desktop has no factory partition, so this installs nothing. It also does not CONSUME anything:
// an earlier version read the caller's first chunk to run the vetting, which took bytes off a
// stream the caller still owned for a check whose real coverage is unit_FirmwareImage driving
// mm::firmware::identify directly. Refusing without touching the source is the honest stub.
bool otaWriteMoonBase(FsWriteSrc, void*, size_t, char* statusBuf, size_t statusBufLen,
                      uint32_t* bytesReadOut) {
    if (statusBuf && statusBufLen > 0) std::snprintf(statusBuf, statusBufLen, "unsupported on desktop");
    if (bytesReadOut) *bytesReadOut = 0;
    return false;
}

bool moonbaseStageInstallUrl(const char*) { return false; }
void moonbaseClearStagedUrl() {}

// Outbound HTTP request (plain HTTP, LAN, no TLS) — see platform.h. Blocking, bounded by a
// receive/send timeout. Builds the request into a stack buffer, connects, sends, reads the
// response, and returns the status code + the body (after the \r\n\r\n). Used by HueDriver
// off the render path.
int httpRequest(const char* method, const char* host, uint16_t port, const char* path,
                const char* reqBody, uint32_t timeoutMs, char* body, size_t bodyLen) {
    if (body && bodyLen) body[0] = '\0';
    if (!method || !host || !path) return 0;

    // One shared budget for the whole request: connect, send, and recv each consume from the same
    // timeoutMs rather than each getting a fresh one (which let the total reach ~3× timeoutMs).
    // `remainingMs()` is the time left, floored at 1ms so a phase never gets a 0 timeout (which
    // means "block forever" for SO_*TIMEO). Tracked as elapsed-since-start (now - start), which is
    // unsigned-wrap-safe across the 32-bit millis() rollover; an absolute `start + timeoutMs`
    // deadline compared with `now >=` would mis-fire when only one side has wrapped.
    const uint32_t start = millis();
    auto remainingMs = [&]() -> uint32_t {
        const uint32_t elapsed = millis() - start;
        return elapsed >= timeoutMs ? 1u : (timeoutMs - elapsed);
    };

    int fd = open_sock(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return 0;
    struct CloseGuard { int f; ~CloseGuard() { close_sock(f); } } guard{fd};

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) return 0;

    // Bound the CONNECT by timeoutMs: a blocking connect to an unreachable host hangs for the OS
    // default (tens of seconds) — and this runs on the driver's tick1s (shared with the render
    // loop), so it must not stall. Connect non-blocking, wait writable via select() up to
    // timeoutMs, then restore blocking for the bounded send/recv (which use SO_*TIMEO below).
    if (make_nonblocking(fd) != 0) return 0;
    int cr = ::connect(sock(fd), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
    // A non-blocking connect that didn't complete immediately reports "in progress":
    // EINPROGRESS on POSIX, WSAEWOULDBLOCK on Winsock. Anything else is a hard failure.
#ifdef _WIN32
    const bool inProgress = (cr != 0 && ::WSAGetLastError() == WSAEWOULDBLOCK);
#else
    const bool inProgress = (cr != 0 && errno == EINPROGRESS);
#endif
    if (cr != 0 && !inProgress) return 0;          // immediate hard failure
    if (cr != 0) {                                 // connect in progress — wait for writable
        fd_set wf; FD_ZERO(&wf); FD_SET(sock(fd), &wf);
        const uint32_t cms = remainingMs();
        timeval ctv{};
        ctv.tv_sec = static_cast<time_t>(cms / 1000);
        // decltype the field, not suseconds_t: tv_usec is `long` on Winsock's timeval (no suseconds_t
        // on Windows) and suseconds_t on POSIX — decltype resolves to the right type on every platform.
        ctv.tv_usec = static_cast<decltype(ctv.tv_usec)>((cms % 1000) * 1000);
        if (::select(static_cast<int>(sock(fd)) + 1, nullptr, &wf, nullptr, &ctv) <= 0) return 0;  // timeout / error
        int soerr = 0; socklen_t len = sizeof(soerr);
        ::getsockopt(sock(fd), SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soerr), &len);
        if (soerr != 0) return 0;                  // connect failed
    }
    if (make_blocking(fd) != 0) return 0;          // back to blocking for the bounded send/recv

    // Bound the request send + response recv with SO_RCVTIMEO/SO_SNDTIMEO, using the time LEFT on
    // the shared deadline (not a fresh timeoutMs) so connect + send + recv together stay within the
    // caller's budget.
    const uint32_t sms = remainingMs();
#ifdef _WIN32
    DWORD tv = sms;
    ::setsockopt(sock(fd), SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
    ::setsockopt(sock(fd), SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
#else
    timeval tv{};
    tv.tv_sec = static_cast<time_t>(sms / 1000);
    tv.tv_usec = static_cast<decltype(tv.tv_usec)>((sms % 1000) * 1000);
    ::setsockopt(sock(fd), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(sock(fd), SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif

    char req[1024];
    const size_t blen = reqBody ? std::strlen(reqBody) : 0;
    int n = blen
        ? std::snprintf(req, sizeof(req),
              "%s %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n"
              "Content-Type: application/json\r\nContent-Length: %zu\r\n\r\n%s",
              method, path, host, blen, reqBody)
        : std::snprintf(req, sizeof(req),
              "%s %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n",
              method, path, host);
    if (n <= 0 || n >= static_cast<int>(sizeof(req))) return 0;
    // Send the whole request — a blocking send can return short under backpressure, so loop
    // until all n bytes are out (retry on a positive partial, fail only on 0 / error).
    for (int off = 0; off < n;) {
        auto w = ::send(sock(fd), req + off, n - off, 0);
        if (w > 0) off += static_cast<int>(w);
        else return 0;
    }

    // Read the response. When the caller wants the body, read into THEIR buffer (so they size it
    // — a Hue /lights body runs several KB) and shift the body to the front. When they don't
    // (body==null, e.g. a fire-and-forget PUT), read into a small local scratch just far enough
    // to get the status line — the request still executes. The status line + headers sit at the
    // front of whatever we read.
    char scratch[256];
    char* buf = body ? body : scratch;
    const size_t cap = body ? bodyLen : sizeof(scratch);
    if (cap < 16) return 0;
    int total = 0;
    while (total < static_cast<int>(cap - 1)) {
        auto r = ::recv(sock(fd), buf + total, cap - 1 - total, 0);
        if (r > 0) total += static_cast<int>(r);
        else break;   // closed or timeout
    }
    buf[total] = '\0';
    if (total < 12 || std::strncmp(buf, "HTTP/1.", 7) != 0) { if (body) body[0] = '\0'; return 0; }
    int status = std::atoi(buf + 9);   // "HTTP/1.1 NNN ..."
    if (body) {
        char* b = std::strstr(body, "\r\n\r\n");
        if (b) std::memmove(body, b + 4, std::strlen(b + 4) + 1);   // drop headers, keep just the body
        else body[0] = '\0';
    }
    return status;
}


// Improv WiFi — no USB-serial path on desktop. The module gates with
// `if constexpr (mm::platform::hasImprov)` and never calls this on desktop;
// the stub exists for compile coverage.
bool improvProvisioningInit(const ImprovDeviceInfo& /*info*/,
                            char* /*ssidOut*/, size_t /*ssidOutLen*/,
                            char* /*passwordOut*/, size_t /*passwordOutLen*/,
                            std::atomic<bool>* /*ready*/,
                            char* statusBuf, size_t statusBufLen,
                            uint8_t* /*txPowerOut*/,
                            std::atomic<bool>* /*txPowerReady*/,
                            char* /*opOut*/, size_t /*opOutLen*/,
                            std::atomic<bool>* /*opReady*/) {
    if (statusBuf && statusBufLen > 0) {
        std::snprintf(statusBuf, statusBufLen, "unsupported on desktop");
    }
    return false;
}

void reboot() {
    // Desktop: the device is the host process. Exit cleanly; the OS user / supervisor
    // can restart it. Matches the "device disappeared from the network" semantics the
    // browser-side WS reconnect logic expects.
    std::printf("platform::reboot() — exiting\n");
    std::fflush(stdout);
    // Exiting the process IS the desktop reboot — there is no firmware to restart into. The
    // mt-unsafe warning is about exit() racing other threads' atexit handlers, which is exactly
    // the abrupt teardown a reboot models.
    // NOLINTNEXTLINE(concurrency-mt-unsafe)
    std::exit(0);
}

// UdpSocket

UdpSocket::~UdpSocket() {
    close();
}

bool UdpSocket::open() {
    if (fd_ >= 0) return true;
    fd_ = open_sock(AF_INET, SOCK_DGRAM, 0);
    if (fd_ < 0) return false;
    // Allow sends to a broadcast address (e.g. 255.255.255.255 for an Art-Net /
    // E1.31 spray to every device on the LAN). Without SO_BROADCAST the OS rejects
    // such a send with EACCES; it has no effect on unicast/multicast sends.
    const int on = 1;
    ::setsockopt(sock(fd_), SOL_SOCKET, SO_BROADCAST,
                 reinterpret_cast<const char*>(&on), sizeof(on));
    return true;
}

bool UdpSocket::connect(const char* ip, uint16_t port) {
    if (fd_ < 0) return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) return false;
    return ::connect(sock(fd_), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0;
}

bool UdpSocket::sendTo(const uint8_t* data, size_t len) {
    if (fd_ < 0) return false;
    return ::send(sock(fd_), reinterpret_cast<const char*>(data), static_cast<int>(len), 0) >= 0;
}

// Test override (see platform.h): forces bind() to fail so a test can drive the failure path without
// relying on the OS to refuse a port — which is not portable (Linux permits the overlapping UDP bind).
static std::atomic<bool> testBindFails{false};
void setTestBindFails(bool fail) { testBindFails.store(fail, std::memory_order_relaxed); }

bool UdpSocket::bind(uint16_t port) {
    if (fd_ < 0) return false;
    if (testBindFails.load(std::memory_order_relaxed)) return false;
    // SO_REUSEADDR semantic split: on POSIX it lets a fresh socket claim a port left in
    // TIME_WAIT (never allows two live binds to overlap). On Winsock its meaning is the
    // opposite of POSIX — two live sockets can bind the same port, so a second bind()
    // returns success instead of the EADDRINUSE the audio-sync retry-backoff logic reads
    // as "port owned by someone else" (unit_AudioService_sync's hog-then-module scenario
    // exercises exactly that). Windows' equivalent-to-POSIX behaviour is the *default*,
    // so on Windows we skip the setsockopt and let a second bind fail naturally.
    //
    // NOTE the outcome is NOT the same on every platform, contrary to what this comment used to
    // claim: on LINUX, SO_REUSEADDR on a UDP socket bound to INADDR_ANY permits an overlapping bind,
    // so a second bind SUCCEEDS. A test that needs a bind to fail must use setTestBindFails(), not a
    // port hog.
#ifndef _WIN32
    int reuse = 1;
    ::setsockopt(sock(fd_), SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<const char*>(&reuse), sizeof(reuse));
#endif
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(sock(fd_), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) return false;
    // Non-blocking so the render loop's drain never stalls waiting for a packet.
    return make_nonblocking(fd_) == 0;
}

int UdpSocket::recvFrom(uint8_t* buf, size_t maxLen, uint8_t srcIp[4]) {
    if (fd_ < 0) return -1;
    sockaddr_in src{};
    socklen_t srcLen = sizeof(src);
    auto n = ::recvfrom(sock(fd_), reinterpret_cast<char*>(buf), static_cast<int>(maxLen), 0,
                        reinterpret_cast<sockaddr*>(&src), &srcLen);
    // 0-byte datagrams and would-block both mean "nothing usable pending".
    if (n <= 0) return -1;
    if (srcIp) std::memcpy(srcIp, &src.sin_addr.s_addr, 4);   // network order = octets
    return static_cast<int>(n);
}

// Join an IPv4 multicast group so the bound socket receives datagrams sent to it. WLED audio
// sync multicasts to 239.0.0.1; without this membership the datagrams never reach the socket.
// INADDR_ANY as the interface lets the stack pick, which is what a single-homed device wants.
bool UdpSocket::joinMulticast(const char* group) {
    if (fd_ < 0 || !group) return false;
    ip_mreq mreq{};
    if (::inet_pton(AF_INET, group, &mreq.imr_multiaddr) != 1) return false;
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    return ::setsockopt(sock(fd_), IPPROTO_IP, IP_ADD_MEMBERSHIP,
                        reinterpret_cast<const char*>(&mreq), sizeof(mreq)) == 0;
}

bool UdpSocket::sendToAddr(const uint8_t ip[4], uint16_t port,
                           const uint8_t* data, size_t len) {
    if (fd_ < 0) return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    std::memcpy(&addr.sin_addr.s_addr, ip, 4);
    return ::sendto(sock(fd_), reinterpret_cast<const char*>(data), static_cast<int>(len), 0,
                    reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) >= 0;
}

void UdpSocket::close() {
    if (fd_ >= 0) {
        close_sock(fd_);
        fd_ = -1;
    }
}

// TcpConnection

TcpConnection::~TcpConnection() {
    close();
}

int TcpConnection::read(uint8_t* buf, size_t maxLen) {
    if (fd_ < 0) return -1;
    // recv() works the same on POSIX and Winsock — the socket is blocking with
    // SO_RCVTIMEO set in TcpServer::accept (Windows takes DWORD ms, POSIX takes
    // struct timeval). After the timeout, recv returns -1 with EAGAIN/EWOULDBLOCK
    // (POSIX) or WSAEWOULDBLOCK (Windows); we translate both to -1 for the caller.
    auto n = ::recv(sock(fd_), reinterpret_cast<char*>(buf), static_cast<int>(maxLen), 0);
    if (n > 0) return static_cast<int>(n);
    if (n == 0) return 0; // peer closed
    if (sockWouldBlock()) return -1; // read timed out, nothing available
    return 0; // error → treat as closed
}

bool TcpConnection::write(const uint8_t* data, size_t len) {
    if (fd_ < 0) return false;
    // Send ALL bytes (blocking retry on a full buffer) — an HTTP response / WS frame must arrive complete.
    // A healthy interface drains in microseconds so the retry rarely spins. Bounded by a wall-clock
    // deadline (mirrors the ESP32 impl): this runs on the render thread, and a stalled peer whose TCP
    // receive window is full would otherwise make send() block forever and hang the loop. On timeout,
    // return false so the caller closes that client instead of wedging the device.
    // TWO bounds, mirroring the ESP32 impl: the stall bound (progress resets it) lets a
    // slow-but-steady transfer finish (a total-only bound truncated large assets under a parallel
    // cold-cache page load); the total bound keeps a byte-trickling peer from holding the loop.
    constexpr uint32_t kWriteStallMs = 2000;
    constexpr uint32_t kWriteTotalMs = 8000;
    const uint32_t start = millis();
    uint32_t lastProgress = start;
    size_t sent = 0;
    while (sent < len) {
        auto n = ::send(sock(fd_), reinterpret_cast<const char*>(data + sent),
                        static_cast<int>(len - sent), 0);
        if (n > 0) {
            sent += static_cast<size_t>(n);
            lastProgress = millis();
        } else if (sockWouldBlock()) {
            const uint32_t now = millis();
            if (now - lastProgress >= kWriteStallMs || now - start >= kWriteTotalMs)
                return false;   // stalled or crawling peer: close it, never hang the loop
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
#ifndef _WIN32
        } else if (errno == EINTR) {
            continue; // interrupted by signal, retry
#endif
        } else {
            return false;
        }
    }
    return true;
}

int TcpConnection::writeSome(const uint8_t* data, size_t len) {
    if (fd_ < 0) return -1;
    if (len == 0) return 0;
    // The accept()ed socket is persistently non-blocking (set in TcpServer::accept), so a
    // plain ::send() never blocks — no toggle needed. A full kernel send buffer surfaces as
    // EWOULDBLOCK, which we report as 0 ("try later"); the caller advances its own offset.
    auto n = ::send(sock(fd_), reinterpret_cast<const char*>(data), static_cast<int>(len), 0);
    if (n > 0) return static_cast<int>(n);
    if (n == 0) return 0;
    if (sockWouldBlock()) return 0;         // buffer full — try later
#ifndef _WIN32
    if (errno == EINTR) return 0;           // interrupted — try later
#endif
    return -1;                              // real socket error
}


bool TcpConnection::connectStart(const char* host, uint16_t port) {
    if (!host || !host[0]) return false;
    close();

    // One bounded DNS lookup (getaddrinfo) up front — resolving is synchronous, but it's the one
    // unavoidable blocking bit; the CONNECT itself then proceeds non-blocking and is polled.
    char portStr[6];
    std::snprintf(portStr, sizeof(portStr), "%u", static_cast<unsigned>(port));
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (::getaddrinfo(host, portStr, &hints, &res) != 0 || !res) return false;
    struct AiGuard { addrinfo* p; ~AiGuard() { if (p) ::freeaddrinfo(p); } } aiGuard{res};

    int fd = open_sock(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) return false;
    if (make_nonblocking(fd) != 0) { close_sock(fd); return false; }
    int cr = ::connect(sock(fd), res->ai_addr, static_cast<int>(res->ai_addrlen));
#ifdef _WIN32
    const bool inProgress = (cr != 0 && ::WSAGetLastError() == WSAEWOULDBLOCK);
#else
    const bool inProgress = (cr != 0 && errno == EINPROGRESS);
#endif
    if (cr != 0 && !inProgress) { close_sock(fd); return false; }   // immediate hard failure
    fd_ = fd;   // in flight (or already connected) — connectPoll() resolves which
    return true;
}

TcpConnection::ConnectResult TcpConnection::connectPoll() {
    if (fd_ < 0) return ConnectResult::Failed;
    // Zero-timeout select: is the socket writable yet? Never blocks.
    // Watch BOTH writability and the exception set: a completed connect signals writable on POSIX,
    // but a FAILED (refused) non-blocking connect signals via the exception set on Winsock — checking
    // only writefds there leaves a refused connect reading Pending until the caller's timeout.
    fd_set wf; FD_ZERO(&wf); FD_SET(sock(fd_), &wf);
    fd_set ef; FD_ZERO(&ef); FD_SET(sock(fd_), &ef);
    timeval zero{};   // 0s / 0us
    const int r = ::select(static_cast<int>(sock(fd_)) + 1, nullptr, &wf, &ef, &zero);
    if (r == 0) return ConnectResult::Pending;                       // neither writable nor errored yet
    if (r < 0)  { close(); return ConnectResult::Failed; }
    // SO_ERROR distinguishes a real connect from an errored one on both platforms.
    int soerr = 0; socklen_t len = sizeof(soerr);
    ::getsockopt(sock(fd_), SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soerr), &len);
    if (soerr != 0) { close(); return ConnectResult::Failed; }
    return ConnectResult::Connected;                                 // socket stays non-blocking
}

void TcpConnection::close() {
    if (fd_ >= 0) {
        close_sock(fd_);
        fd_ = -1;
    }
}

// TcpServer

TcpServer::~TcpServer() {
    close();
}

bool TcpServer::open(uint16_t port) {
    if (fd_ >= 0) return true;
    fd_ = open_sock(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) return false;

    int opt = 1;
    setsockopt(sock(fd_), SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&opt), sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (::bind(sock(fd_), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close_sock(fd_);
        fd_ = -1;
        return false;
    }

    if (::listen(sock(fd_), 8) < 0) {
        close_sock(fd_);
        fd_ = -1;
        return false;
    }

    make_nonblocking(fd_);

    return true;
}

TcpConnection TcpServer::accept() {
    if (fd_ < 0) return TcpConnection();
#ifdef _WIN32
    SOCKET client = ::accept(sock(fd_), nullptr, nullptr);
    if (client == INVALID_SOCKET) return TcpConnection();
    int clientFd = static_cast<int>(client);
    // NON-BLOCKING (see the POSIX branch below for the full rationale): a blocking recv on
    // the single-loop server stalls the whole render loop. make_nonblocking → recv returns
    // WSAEWOULDBLOCK → read() reports -1 ("nothing yet") immediately, never blocking.
    make_nonblocking(clientFd);
#else
    int clientFd = ::accept(fd_, nullptr, nullptr);
    if (clientFd < 0) return TcpConnection();
    // NON-BLOCKING client socket. The HTTP server is serviced from the single render loop,
    // so a blocking recv()'s timeout (we used 2 s) froze the WHOLE loop whenever a request's
    // bytes hadn't landed the instant accept() returned — UI to a crawl. Non-blocking makes
    // read() return -1 ("nothing yet") immediately, so the loop never stalls; the request
    // (which lands within ~1 ms on localhost/LAN) is read across a few rapid retries in
    // handleConnection. recv returns EWOULDBLOCK → -1, matching read()'s contract.
    make_nonblocking(clientFd);
#endif
    return TcpConnection(clientFd);
}

void TcpServer::close() {
    if (fd_ >= 0) {
        close_sock(fd_);
        fd_ = -1;
    }
}

// ---------------------------------------------------------------------------
// RMT WS2812 on the host: accepted and counted, not refused.
//
// Same rule as the parallel buses above (architecture.md § Platform abstraction). Refusing here
// made RmtLedDriver inert off device, so nothing in it could be tested on a host.
//
// RMT is symbol-based rather than buffer-based, so there is nothing to hand back: the driver owns
// the symbol array and this seam only has to accept it. The resolution is echoed so the driver's
// timing arithmetic (which divides by it) works on real numbers instead of zero.
// ---------------------------------------------------------------------------
namespace {
struct HostRmt { uint32_t resolutionHz = 0; };
HostRmt* hostRmt(void*& impl) {
    if (!impl) impl = new HostRmt();
    return static_cast<HostRmt*>(impl);
}
}  // namespace

bool rmtWs2812Init(RmtWs2812Handle& h, uint8_t /*gpio*/, uint32_t resolutionHz,
                   bool /*invert*/) {
    // A zero resolution would make the driver divide by zero when it converts nanoseconds to
    // ticks — refuse it here rather than hand back a channel that cannot be used.
    if (resolutionHz == 0) return false;
    hostRmt(h.impl)->resolutionHz = resolutionHz;
    return true;
}
uint32_t rmtWs2812Resolution(const RmtWs2812Handle& h) MM_NONBLOCKING {
    return h.impl ? static_cast<HostRmt*>(h.impl)->resolutionHz : 0;
}
bool rmtWs2812Transmit(RmtWs2812Handle& h, const uint32_t* symbols,
                       size_t symbolCount) {
    if (!h.impl || !symbols || symbolCount == 0) return false;
    return true;
}
bool rmtWs2812Wait(RmtWs2812Handle& /*h*/, uint32_t /*timeoutMs*/) { return true; }
void rmtWs2812Deinit(RmtWs2812Handle& h) {
    delete static_cast<HostRmt*>(h.impl);
    h.impl = nullptr;
}
size_t rmtWs2812RxCapture(uint8_t /*gpio*/, uint32_t /*resolutionHz*/,
                          uint32_t* /*outSymbols*/, size_t /*maxSymbols*/,
                          uint32_t /*timeoutMs*/) {
    return 0;
}
RmtLoopbackResult rmtWs2812Loopback(uint8_t /*txGpio*/, uint8_t /*rxGpio*/) {
    return {};   // not supported off ESP32
}
RmtLoopbackResult rmtWs2812LoopbackFrame(uint8_t /*txGpio*/, uint8_t /*rxGpio*/,
                                         uint16_t /*lights*/, uint8_t /*channels*/) {
    return {};   // not supported off ESP32
}
RmtLoopbackResult ws2812LoopbackRide(uint16_t /*rxGpio*/, const uint8_t* /*sent*/, uint8_t /*sentLen*/,
                                     size_t /*dataBytes*/, uint8_t /*rowBits*/,
                                     uint8_t /*clockMultiplier*/) {
    return {};   // no RMT-RX capture off ESP32
}

// ---------------------------------------------------------------------------
// Parallel-WS2812 buses on desktop: REAL MEMORY, no silicon.
//
// The repo's rule is that everything runs on the desktop build — the platform layer simply has
// no hardware behind the call. These used to return false/nullptr, which made every parallel
// backend report failure, so ParallelLedDriver's ~2500-line body never executed off-device: not
// runnable, not unit-testable, and invisible to every AST-based check.
//
// So the bus is implemented against a heap buffer. `init` allocates and zeroes, `Buffer` hands
// back writable memory, `Transmit` records the byte count, `Wait` returns immediately. Everything
// ABOVE the seam is then the same code that runs on hardware — the driver encodes real WS2812 bit
// patterns into a real buffer — and only the DMA hand-off is absent.
//
// What is deliberately NOT modelled: timing, wire protocol, pin state, and loopback capture.
// Those need silicon, and faking them would make the driver's self-test lie about hardware it
// never touched.
// ---------------------------------------------------------------------------
namespace {

/// One memory-backed parallel bus. Shared by the i80, MoonI80 and Parlio seams below — they are
/// three DMA peripherals for the same job, and off-device the job is "hold a frame".
struct HostBus {
    std::vector<uint8_t> buf[2];
    size_t capacity = 0;

    bool init(size_t bytes, bool wantSecond) {
        if (bytes == 0) return false;
        capacity = bytes;
        buf[0].assign(bytes, 0);
        if (wantSecond) buf[1].assign(bytes, 0);
        else            buf[1].clear();
        return true;
    }
    uint8_t* buffer(uint8_t i) {
        if (i > 1 || buf[i].empty()) return nullptr;
        return buf[i].data();
    }
    bool transmit(uint8_t i, size_t bytes) {
        if (i > 1 || buf[i].empty() || bytes > capacity) return false;
        return true;
    }
};

HostBus* hostBus(void*& impl) {
    if (!impl) impl = new HostBus();
    return static_cast<HostBus*>(impl);
}
void freeHostBus(void*& impl) {
    delete static_cast<HostBus*>(impl);
    impl = nullptr;
}

}  // namespace

const char* i80Ws2812LastError() { return nullptr; }   // the emulated bus never refuses for a cause
bool i80Ws2812SharedBusFree() { return false; }        // and shares no peripheral, so never retries
bool i80Ws2812Init(I80Ws2812Handle& h, const uint16_t* /*dataPins*/,
                   uint8_t /*laneCount*/, uint16_t /*wrGpio*/, uint16_t /*dcGpio*/,
                   size_t bufferBytes, bool wantSecondBuffer,
                   uint8_t /*clockMultiplier*/) {
    if (bufferBytes == 0) return false;   // refuse before allocating, as the RMT seam does
    return hostBus(h.impl)->init(bufferBytes, wantSecondBuffer);
}
uint8_t* i80Ws2812Buffer(const I80Ws2812Handle& h, uint8_t buffer) {
    return h.impl ? static_cast<HostBus*>(h.impl)->buffer(buffer) : nullptr;
}
size_t i80Ws2812BufferCapacity(const I80Ws2812Handle& h) {
    return h.impl ? static_cast<HostBus*>(h.impl)->capacity : 0;
}
bool i80Ws2812Transmit(I80Ws2812Handle& h, uint8_t buffer, size_t bytes) {
    return h.impl && static_cast<HostBus*>(h.impl)->transmit(buffer, bytes);
}
// True, not false: the driver reads a false as "the previous frame never completed" and holds
// the next one back, which would stall the render path on a bus that is never busy.
bool i80Ws2812Wait(I80Ws2812Handle& /*h*/, uint8_t /*buffer*/, uint32_t /*timeoutMs*/) { return true; }
uint32_t i80Ws2812LastTransmitUs(const I80Ws2812Handle& /*h*/) { return 0; }
void i80Ws2812Deinit(I80Ws2812Handle& h) { freeHostBus(h.impl); }
RmtLoopbackResult i80Ws2812Loopback(const uint16_t* /*dataPins*/, uint8_t /*laneCount*/,
                                    uint16_t /*wrGpio*/, uint16_t /*dcGpio*/,
                                    uint16_t /*rxGpio*/, const uint8_t* /*frame*/,
                                    size_t /*frameBytes*/, size_t /*dataBytes*/,
                                    uint8_t /*rowBits*/, uint8_t /*clockMultiplier*/) {
    return {};   // not supported off the S3
}

// MoonI80 (our own LCD_CAM DMA driver, ADR-0014) — the same memory-backed bus as the esp_lcd
// family above. The RING path stays inert: it is a GDMA construct with no host equivalent, so a
// driver that would stream on device runs whole-frame here (busInitRing returns false and the
// orchestrator falls back, exactly as its contract specifies).
bool moonI80Ws2812Init(MoonI80Ws2812Handle& h, const uint16_t* /*dataPins*/,
                       uint8_t /*laneCount*/, uint16_t /*wrGpio*/,
                       size_t bufferBytes, bool wantSecondBuffer,
                       uint8_t /*clockMultiplier*/) {
    if (bufferBytes == 0) return false;   // refuse before allocating, as the other seams do
    return hostBus(h.impl)->init(bufferBytes, wantSecondBuffer);
}
// Ring mode is a GDMA construct with no host equivalent — inert here, bench-verified on the S3, exactly
// like the whole-frame path above. A driver that would pick the ring on device stays whole-frame on host.
bool moonI80Ws2812InitRing(MoonI80Ws2812Handle& /*h*/, const uint16_t* /*dataPins*/,
                           uint8_t /*laneCount*/, uint16_t /*wrGpio*/, size_t /*rowBytes*/,
                           uint32_t /*totalRows*/, uint32_t /*rowsPerBuf*/, uint8_t /*ringBufs*/,
                           uint8_t /*padUs*/, uint8_t /*clockMultiplier*/, MoonI80EncodeFn /*encode*/,
                           void* /*user*/) {
    return false;
}
bool moonI80Ws2812TransmitRing(MoonI80Ws2812Handle& /*h*/) { return false; }
void moonI80SetShiftClockDiv(uint8_t /*div*/) {}
void moonI80Ws2812PrimeRange(MoonI80Ws2812Handle& /*h*/, uint8_t /*bufLo*/, uint8_t /*bufHi*/) {}
bool moonI80Ws2812ArmRing(MoonI80Ws2812Handle& /*h*/) { return false; }
bool moonI80Ws2812IsRing(const MoonI80Ws2812Handle& /*h*/) { return false; }
bool moonI80Ws2812InternalFits(size_t /*bytes*/) { return false; }
uint8_t* moonI80Ws2812Buffer(const MoonI80Ws2812Handle& h, uint8_t buffer) {
    return h.impl ? static_cast<HostBus*>(h.impl)->buffer(buffer) : nullptr;
}
size_t moonI80Ws2812BufferCapacity(const MoonI80Ws2812Handle& h) {
    return h.impl ? static_cast<HostBus*>(h.impl)->capacity : 0;
}
bool moonI80Ws2812Transmit(MoonI80Ws2812Handle& h, uint8_t buffer, size_t bytes) {
    return h.impl && static_cast<HostBus*>(h.impl)->transmit(buffer, bytes);
}
bool moonI80Ws2812Wait(MoonI80Ws2812Handle& /*h*/, uint8_t /*buffer*/, uint32_t /*timeoutMs*/) { return true; }
uint32_t moonI80Ws2812LastTransmitUs(const MoonI80Ws2812Handle& /*h*/) { return 0; }
MoonI80RingStats moonI80Ws2812RingStats(const MoonI80Ws2812Handle& /*h*/) { return {}; }
void moonI80Ws2812Deinit(MoonI80Ws2812Handle& h) { freeHostBus(h.impl); }
RmtLoopbackResult moonI80Ws2812Loopback(const uint16_t* /*dataPins*/, uint8_t /*laneCount*/,
                                        uint16_t /*wrGpio*/,
                                        uint16_t /*rxGpio*/, const uint8_t* /*frame*/,
                                        size_t /*frameBytes*/, size_t /*dataBytes*/,
                                        uint8_t /*rowBits*/, uint8_t /*clockMultiplier*/,
                                        uint32_t /*ringRows*/, uint32_t /*ringBufs*/,
                                        bool /*useRing*/) {
    return {};   // not supported off LCD_CAM
}
RmtLoopbackResult moonI80Ws2812LoopbackRide(uint16_t /*rxGpio*/, const uint8_t* /*sent*/,
                                            uint8_t /*sentLen*/, size_t /*dataBytes*/,
                                            uint8_t /*rowBits*/, uint8_t /*clockMultiplier*/) {
    return {};   // not supported off LCD_CAM
}

// Parlio WS2812 — the same memory-backed bus. No Parlio silicon here, but the driver runs and
// its sizing/slicing is host-pinned by the driver tests.
bool parlioWs2812Init(ParlioWs2812Handle& h, const uint16_t* /*dataPins*/,
                      uint8_t /*laneCount*/, uint32_t /*pclkHz*/, size_t bufferBytes,
                      bool wantSecondBuffer) {
    if (bufferBytes == 0) return false;   // refuse before allocating, as the other seams do
    return hostBus(h.impl)->init(bufferBytes, wantSecondBuffer);
}
uint8_t* parlioWs2812Buffer(const ParlioWs2812Handle& h, uint8_t buffer) {
    return h.impl ? static_cast<HostBus*>(h.impl)->buffer(buffer) : nullptr;
}
size_t parlioWs2812BufferCapacity(const ParlioWs2812Handle& h) {
    return h.impl ? static_cast<HostBus*>(h.impl)->capacity : 0;
}
// The desktop host emulates the bus in ordinary memory, so there is no single-transfer ceiling to
// declare — 0 is the "no bound" contract dmaBudgetBytes() reads, matching every other host-side
// Parlio stub here.
size_t parlioMaxTransferBytes() { return 0; }
bool parlioWs2812Transmit(ParlioWs2812Handle& h, uint8_t buffer, size_t bytes) {
    return h.impl && static_cast<HostBus*>(h.impl)->transmit(buffer, bytes);
}
bool parlioWs2812Wait(ParlioWs2812Handle& /*h*/, uint8_t /*buffer*/, uint32_t /*timeoutMs*/) { return true; }
uint32_t parlioWs2812LastTransmitUs(const ParlioWs2812Handle& /*h*/) { return 0; }
void parlioWs2812Deinit(ParlioWs2812Handle& h) { freeHostBus(h.impl); }
RmtLoopbackResult parlioWs2812Loopback(const uint16_t* /*dataPins*/, uint8_t /*laneCount*/,
                                       uint16_t /*rxGpio*/, const uint8_t* /*frame*/,
                                       size_t /*frameBytes*/, size_t /*dataBytes*/,
                                       uint8_t /*rowBits*/) {
    return {};   // not supported off the P4
}

// Audio codec + capture live in platform_desktop_audio.cpp (the miniaudio TU): codec is a
// succeed-no-op (nothing to bring up), the mic seam reads the OS capture device.

// FFT kernel: iterative radix-2 Cooley-Tukey (the textbook in-place decimation-in-time
// form), the production desktop kernel now that live capture runs 512-point blocks ~43x/s
// on the render tick (the previous naive O(n^2) DFT was, per its own comment, only fast
// enough for host tests). Identical contract: n a power of two, outMag[0..n/2) filled with
// unnormalized bin magnitudes sqrt(re^2+im^2); numerically equivalent to the DFT (pinned by
// unit_platform_audiofft against a DFT reference).
void audioFft(const float* windowed, size_t n, float* outMag) {
    if (!windowed || !outMag || n == 0 || (n & (n - 1)) != 0) return;
    constexpr size_t kMaxN = 4096;
    if (n > kMaxN) return;
    static float re[kMaxN], im[kMaxN];   // scratch; render-thread only, like the ESP32 kernel's

    // Bit-reversal permutation while loading the input.
    const int bits = static_cast<int>(std::countr_zero(n));
    for (size_t i = 0; i < n; i++) {
        size_t r = 0;
        for (int b = 0; b < bits; b++) r |= ((i >> b) & 1u) << (bits - 1 - b);
        re[r] = windowed[i];
        im[r] = 0.0f;
    }

    // Butterflies: stages of doubling span, twiddles advanced per group.
    for (size_t len = 2; len <= n; len <<= 1) {
        const float ang = -2.0f * std::numbers::pi_v<float> / static_cast<float>(len);
        const float wRe = std::cos(ang), wIm = std::sin(ang);
        for (size_t i = 0; i < n; i += len) {
            float curRe = 1.0f, curIm = 0.0f;
            for (size_t j = 0; j < len / 2; j++) {
                const size_t a = i + j, b = a + len / 2;
                const float tRe = re[b] * curRe - im[b] * curIm;
                const float tIm = re[b] * curIm + im[b] * curRe;
                re[b] = re[a] - tRe; im[b] = im[a] - tIm;
                re[a] += tRe;        im[a] += tIm;
                const float nRe = curRe * wRe - curIm * wIm;
                curIm = curRe * wIm + curIm * wRe;
                curRe = nRe;
            }
        }
    }

    for (size_t k = 0; k < n / 2; k++) outMag[k] = std::sqrt(re[k] * re[k] + im[k] * im[k]);
}

// No I2C bus on the desktop host — report it as unavailable (the sentinel), the same as
// an I2C-less ESP32 target, so the module shows "bus unavailable" rather than a misleading
// "0 devices found" (which means "scanned a real bus, nothing ACKed").
size_t i2cScan(uint16_t /*sda*/, uint16_t /*scl*/, uint8_t* /*out*/, size_t /*maxOut*/) {
    return kI2cBusUnavailable;
}

// No IR receiver on the host: the seam is a no-op so InfraredService runs (its buttons still work
// through Scheduler::setControl); reception is ESP32-only.
// GPIO on a host has no pins, so reads come from what a test injected. That is the point: the
// button/pedal logic (debounce, edge, latch) is ordinary code and gets tested here, leaving only
// the electrical half for the bench.
namespace {
// A flat table rather than a map: a pin number IS the index, there are at most 48 of them, and this
// allocates nothing.
constexpr uint8_t kMaxGpio = 48;
bool g_gpioLevel[kMaxGpio] = {};
}

bool gpioInputBegin(uint8_t gpio, GpioPull pull) {
    if (gpio >= kMaxGpio) return false;
    // The PULL sets the resting level, as it does on a board: a pull-up idles HIGH, a pull-down
    // idles LOW. Without this every pin idled LOW, which an active-low button reads as HELD, so a
    // desktop with no hardware reported a phantom press the moment a row named a pin. A test that
    // wants a different level still calls setTestGpioLevel after this.
    g_gpioLevel[gpio] = (pull == GpioPull::Up);
    return true;
}

bool gpioRead(uint8_t gpio) { return gpio < kMaxGpio && g_gpioLevel[gpio]; }

bool gpioWrite(uint8_t gpio, bool high) {
    // A write is observable through gpioRead, so a test can drive a pin and read back what a module
    // put there (a relay enable, MoonLive's write-then-read hello world).
    if (gpio >= kMaxGpio) return false;
    g_gpioLevel[gpio] = high;
    return true;
}

void setTestGpioLevel(uint8_t gpio, bool level) { if (gpio < kMaxGpio) g_gpioLevel[gpio] = level; }
void clearTestGpioLevel() { for (bool& b : g_gpioLevel) b = false; }

// --- ADC ---
// The desktop has no converter, so a read reports whatever a test injected. Same arrangement as the
// GPIO level above: a pedal's mapping, its min/max/invert and its smoothing are ordinary logic, and
// this is what lets all of it be pinned on the host with no hardware attached.
namespace {
uint16_t g_adcValue[kMaxGpio] = {};
// Millivolts are injected SEPARATELY from the raw count rather than derived from it. On a board the
// two are related by the chip's own eFuse curve, which a host cannot reproduce, so deriving one here
// would let a test pass against an arithmetic relationship that does not hold on hardware.
uint16_t g_adcMv[kMaxGpio] = {};
}

bool adcRead(uint8_t gpio, uint16_t& raw) {
    if (gpio >= kMaxGpio) return false;
    raw = g_adcValue[gpio];
    return true;
}

// The ESP32's 12-bit full scale, reported here too so a host test scales exactly as the board does:
// a mapping verified against 4095 on the desktop cannot then behave differently on a device.
uint16_t adcMaxCount() { return 4095; }

void setTestAdcValue(uint8_t gpio, uint16_t raw) { if (gpio < kMaxGpio) g_adcValue[gpio] = raw; }
void clearTestAdcValue() { for (uint16_t& v : g_adcValue) v = 0; for (uint16_t& v : g_adcMv) v = 0; }

bool adcReadMv(uint8_t gpio, uint16_t& mv) {
    if (gpio >= kMaxGpio) return false;
    mv = g_adcMv[gpio];
    return true;
}

void setTestAdcMv(uint8_t gpio, uint16_t mv) { if (gpio < kMaxGpio) g_adcMv[gpio] = mv; }

bool irRead(uint16_t /*pin*/, uint32_t& /*codeOut*/) { return false; }
void irStop() {}   // no IR hardware on desktop
bool irChannelReady(uint16_t /*pin*/) { return true; }   // no channel to fail on desktop


// --- NDI video output ---------------------------------------------------------------------------
//
// projectMM as an NDI source (contract + the licensing reason for this shape: platform.h § NDI).
// The runtime is resolved on demand and NEVER linked, bundled, or its headers included — the same
// arrangement as the Npcap block above, for the same GPL-3 reason. The user installs the NDI
// runtime; a machine without it builds and runs identically and reports the feature unavailable.
//
// The three declarations below are transcribed from the SDK's own public headers
// (Processing.NDI.structs.h and Processing.NDI.Send.h). Getting a field's type or ORDER wrong here
// is a silent crash or a skewed image rather than a compile error, because these are passed by
// pointer into a binary that was built against the real definitions. They are quoted verbatim in
// the plan (docs/history/plans) with their source, and must not be "tidied".
namespace {

using NdiSendInstance = void*;

// Processing.NDI.structs.h — NDIlib_video_frame_v2_t, verbatim field order.
struct NdiVideoFrameV2 {
    int         xres, yres;
    int         FourCC;                 // NDIlib_FourCC_video_type_e — an int-sized enum
    int         frame_rate_N, frame_rate_D;
    float       picture_aspect_ratio;
    int         frame_format_type;      // NDIlib_frame_format_type_e
    int64_t     timecode;
    uint8_t*    p_data;
    union { int line_stride_in_bytes; int data_size_in_bytes; };
    const char* p_metadata;
    int64_t     timestamp;
};

// Processing.NDI.Send.h — NDIlib_send_create_t, verbatim field order.
struct NdiSendCreate {
    const char* p_ndi_name;
    const char* p_groups;
    bool        clock_video, clock_audio;
};

// NDI_LIB_FOURCC('B','G','R','X') — X, not A: projectMM has no alpha to send, and an ignored
// alpha channel is exactly what the X variants mean. Little-endian packing, as the macro builds it.
constexpr int kFourCCBgrx = 'B' | ('G' << 8) | ('R' << 16) | (static_cast<int>('X') << 24);
constexpr int kFrameFormatProgressive = 1;   // NDIlib_frame_format_type_progressive

using NdiInitFn       = bool (*)();
using NdiDestroyFn    = void (*)();
using NdiSendCreateFn = NdiSendInstance (*)(const NdiSendCreate*);
using NdiSendVideoFn  = void (*)(NdiSendInstance, const NdiVideoFrameV2*);
using NdiSendDestroyFn= void (*)(NdiSendInstance);

NdiInitFn        ndiInit_        = nullptr;
NdiDestroyFn     ndiDestroy_     = nullptr;
NdiSendCreateFn  ndiSendCreate_  = nullptr;
NdiSendVideoFn   ndiSendVideo_   = nullptr;
NdiSendDestroyFn ndiSendDestroy_ = nullptr;

// Test capture (platform.h § NDI test seam). Recording is OFF unless a test turns it on, so a
// desktop build with a real runtime behaves exactly as it would in production.
struct NdiCapturedFrame { uint16_t w, h; uint8_t fps; std::vector<uint8_t> rgb; };
NdiTestMode                  ndiTestMode_ = NdiTestMode::Off;
std::vector<NdiCapturedFrame> ndiCaptured_;
std::string                  ndiCapturedName_;

void*           ndiLib_    = nullptr;
NdiSendInstance ndiSender_ = nullptr;
std::string     ndiName_;                 // owned: NDIlib_send_create_t holds the pointer, not a copy
std::vector<uint8_t> ndiFrame_;           // BGRX staging, resized only on a geometry change

/// The runtime's file name per platform, tried in order. The SONAME first, then the plain name a
/// manual install leaves; Windows resolves through PATH, which the NDI installer sets.
const char* const kNdiLibNames[] = {
#if defined(_WIN32)
    "Processing.NDI.Lib.x64.dll", "Processing.NDI.Lib.x86.dll",
#elif defined(__APPLE__)
    // NDI Tools for macOS ships the runtime INSIDE its app bundles rather than installing a
    // system-wide dylib, so a plain name resolves nothing however complete the install is. The
    // bundle paths are tried by name (verified to export the send API on a real NDI Tools install);
    // `libndi_advanced` is the file NDI Tools ships, `libndi` the one bundled with Resolume.
    "libndi.dylib", "/usr/local/lib/libndi.dylib", "/opt/homebrew/lib/libndi.dylib",
    "/Applications/NDI Video Monitor.app/Contents/Frameworks/libndi_advanced.dylib",
    "/Applications/NDI Studio Monitor.app/Contents/Frameworks/libndi_advanced.dylib",
    "/Applications/NDI Discovery.app/Contents/Frameworks/libndi_advanced.dylib",
    "/Applications/Resolume Arena/libndi.dylib",
    "/Applications/Resolume Avenue/libndi.dylib",
#else
    "libndi.so.5", "libndi.so.6", "libndi.so",
#endif
};

bool ndiLoad() {
    if (ndiSendVideo_) return true;      // already resolved
    if (!ndiLib_) {
        for (const char* name : kNdiLibNames) {
#if defined(_WIN32)
            ndiLib_ = reinterpret_cast<void*>(::LoadLibraryA(name));
#else
            ndiLib_ = ::dlopen(name, RTLD_LAZY | RTLD_LOCAL);
#endif
            if (ndiLib_) break;
        }
    }
    if (!ndiLib_) return false;
    auto sym = [](void* m, const char* n) -> void* {
#if defined(_WIN32)
        return reinterpret_cast<void*>(::GetProcAddress(static_cast<HMODULE>(m), n));
#else
        return ::dlsym(m, n);
#endif
    };
    ndiInit_         = reinterpret_cast<NdiInitFn>(sym(ndiLib_, "NDIlib_initialize"));
    ndiDestroy_      = reinterpret_cast<NdiDestroyFn>(sym(ndiLib_, "NDIlib_destroy"));
    ndiSendCreate_   = reinterpret_cast<NdiSendCreateFn>(sym(ndiLib_, "NDIlib_send_create"));
    ndiSendVideo_    = reinterpret_cast<NdiSendVideoFn>(sym(ndiLib_, "NDIlib_send_send_video_v2"));
    ndiSendDestroy_  = reinterpret_cast<NdiSendDestroyFn>(sym(ndiLib_, "NDIlib_send_destroy"));
    if (!ndiInit_ || !ndiSendCreate_ || !ndiSendVideo_ || !ndiSendDestroy_) {
        ndiSendVideo_ = nullptr;         // treat a partial resolve as absent
        return false;
    }
    // NDIlib_initialize returns false when the CPU is unsupported — a real "cannot use it" that
    // must not read as "installed and working".
    if (!ndiInit_()) { ndiSendVideo_ = nullptr; return false; }
    return true;
}

}  // namespace

bool ndiAvailable() {
    if (ndiTestMode_ == NdiTestMode::ForceAvailable) return true;
    if (ndiTestMode_ == NdiTestMode::ForceMissing) return false;
    return ndiLoad();
}

bool ndiSenderOpen(const char* name) {
    if (ndiTestMode_ == NdiTestMode::ForceMissing) return false;
    if (ndiTestMode_ == NdiTestMode::ForceAvailable) { ndiCapturedName_ = (name && name[0]) ? name : "projectMM"; return true; }
    if (!ndiLoad()) return false;
    ndiSenderClose();
    ndiName_ = (name && name[0]) ? name : "projectMM";
    NdiSendCreate create{};
    create.p_ndi_name = ndiName_.c_str();   // the string must outlive the sender, hence ndiName_
    create.p_groups   = nullptr;
    // FALSE deliberately: clock_video makes send_send_video_v2 BLOCK to pace the caller, and this
    // is called from the render thread which must never block. The driver already rate-limits to
    // its fps control, so the pacing is ours to do.
    create.clock_video = false;
    create.clock_audio = false;             // no audio is sent
    ndiSender_ = ndiSendCreate_(&create);
    return ndiSender_ != nullptr;
}

void ndiSenderClose() {
    if (ndiTestMode_ != NdiTestMode::Off) { ndiCapturedName_.clear(); return; }
    if (ndiSender_) { ndiSendDestroy_(ndiSender_); ndiSender_ = nullptr; }
    ndiFrame_.clear();
    ndiFrame_.shrink_to_fit();
}

bool ndiSendFrame(const uint8_t* rgb, uint16_t w, uint16_t h, uint8_t fps) {
    if (!rgb || w == 0 || h == 0) return false;
    if (ndiTestMode_ == NdiTestMode::ForceAvailable) {
        // Record what the driver produced, tight RGB, exactly as handed over.
        NdiCapturedFrame f{w, h, fps, {}};
        f.rgb.assign(rgb, rgb + static_cast<size_t>(w) * h * 3);
        ndiCaptured_.push_back(std::move(f));
        return true;
    }
    if (!ndiSender_) return false;
    const size_t pixels = static_cast<size_t>(w) * h;
    ndiFrame_.resize(pixels * 4);           // no-op once warm; the only allocation, never per frame
    // RGB -> BGRX. The 4th byte is the ignored X, written once as 0xFF so a receiver that reads it
    // as alpha sees opaque rather than transparent.
    for (size_t i = 0; i < pixels; ++i) {
        ndiFrame_[i * 4 + 0] = rgb[i * 3 + 2];
        ndiFrame_[i * 4 + 1] = rgb[i * 3 + 1];
        ndiFrame_[i * 4 + 2] = rgb[i * 3 + 0];
        ndiFrame_[i * 4 + 3] = 0xFF;
    }
    NdiVideoFrameV2 f{};
    f.xres = w;
    f.yres = h;
    f.FourCC = kFourCCBgrx;
    f.frame_rate_N = fps ? fps : 30;
    f.frame_rate_D = 1;
    f.picture_aspect_ratio = 0.0f;          // 0 = square pixels, which a light grid has
    f.frame_format_type = kFrameFormatProgressive;
    f.timecode = INT64_MAX;                 // NDIlib_send_timecode_synthesize: let NDI stamp it
    f.p_data = ndiFrame_.data();
    f.line_stride_in_bytes = static_cast<int>(w) * 4;
    ndiSendVideo_(ndiSender_, &f);          // synchronous, and clocked by clock_video above
    return true;
}

void setTestNdiMode(NdiTestMode mode) {
    ndiTestMode_ = mode;
    if (mode != NdiTestMode::ForceAvailable) { ndiCaptured_.clear(); ndiCapturedName_.clear(); }
}
size_t ndiTestFrameCount() { return ndiCaptured_.size(); }
uint16_t ndiTestFrameWidth(size_t i)  { return i < ndiCaptured_.size() ? ndiCaptured_[i].w : 0; }
uint16_t ndiTestFrameHeight(size_t i) { return i < ndiCaptured_.size() ? ndiCaptured_[i].h : 0; }
uint8_t  ndiTestFrameFps(size_t i)    { return i < ndiCaptured_.size() ? ndiCaptured_[i].fps : 0; }
const uint8_t* ndiTestFrameData(size_t i) {
    return i < ndiCaptured_.size() ? ndiCaptured_[i].rgb.data() : nullptr;
}
const char* ndiTestSenderName() { return ndiCapturedName_.c_str(); }
void ndiTestClearFrames() { ndiCaptured_.clear(); }


// --- HLS encoder (ffmpeg pipe) -----------------------------------------------------------------
//
// One spawned ffmpeg, stdin piped, argv from the driver (platform.h owns the contract). A
// platform writer thread does the BLOCKING pipe writes on every OS while callers only enqueue
// whole frames into a fixed reuse ring, so the render tick never touches the pipe and no
// per-OS non-blocking trickery is needed. POSIX spawns via posix_spawn with default-CLOEXEC;
// Windows via CreateProcess. Test seam mirrors NdiTestMode so CI never needs an ffmpeg.

namespace {
// Threading model: encoderStart/Stop/Running are LIFECYCLE calls, made only from the render
// task (prepare/release/tick1s), so they need no lock among themselves. encoderWrite crosses
// threads (the encode worker) and the writer thread consumes: those three share encMutex_,
// which guards only the queue and the dead/stop flags, never a blocking write.
std::mutex encMutex_;
std::condition_variable encCv_;
// A fixed ring of REUSED frame slots, whole frames only (tearing is structurally out): the
// enqueue path must not heap-allocate per frame (assign() reuses each slot's capacity after
// the first lap), and 3 slots of burst absorption is the drop-newest boundary.
constexpr size_t kEncQueueMax = 3;
std::vector<uint8_t> encSlots_[kEncQueueMax];
size_t encHead_ = 0;    // slot the writer consumes next
size_t encCount_ = 0;   // filled slots
std::thread encWriter_;
bool encWriterStop_ = false;
bool encWriterDead_ = false;                  // the writer saw EPIPE/error: the process is gone
EncoderTestMode encTestMode_ = EncoderTestMode::Off;
constexpr int kEncTestWriteNormal = 1;   // sentinel: record normally (real results are 0/-1/len)
int encTestWriteResult_ = kEncTestWriteNormal;
std::vector<std::vector<uint8_t>> encCaptured_;
std::string encCapturedArgs_;

#ifdef _WIN32
HANDLE encProcess_ = nullptr;
HANDLE encStdin_ = nullptr;
#else
pid_t encPid_ = -1;
int encStdin_ = -1;
#endif
}  // namespace


// Stop the child and the writer, deadlock-free: signal stop, TERM the child FIRST (a writer
// blocked in write() only reliably unblocks when the read side dies: EPIPE), join, then close
// stdin and reap with a short grace before SIGKILL. Called only from the render task.
static void stopEncoderProcess() {
    if (encTestMode_ != EncoderTestMode::Off) return;
    {
        std::lock_guard<std::mutex> lk(encMutex_);
        encWriterStop_ = true;
        // The ring counters are NOT reset here: the writer may be mid-write on the head slot,
        // and a producer racing this stop must keep seeing that slot as occupied. encoderStart
        // resets the ring under the lock after the join, when nothing can touch it.
        encCv_.notify_all();
    }
#ifdef _WIN32
    if (encProcess_) TerminateProcess(encProcess_, 0);
    if (encWriter_.joinable()) encWriter_.join();
    if (encStdin_) { CloseHandle(encStdin_); encStdin_ = nullptr; }
    if (encProcess_) { WaitForSingleObject(encProcess_, 500); CloseHandle(encProcess_); encProcess_ = nullptr; }
#else
    if (encPid_ >= 0) ::kill(encPid_, SIGTERM);
    if (encWriter_.joinable()) encWriter_.join();
    if (encStdin_ >= 0) { ::close(encStdin_); encStdin_ = -1; }
    if (encPid_ >= 0) {
        for (int i = 0; i < 20; i++) {                   // ~200 ms of graceful exit
            if (::waitpid(encPid_, nullptr, WNOHANG) == encPid_) { encPid_ = -1; break; }
            ::usleep(10 * 1000);
        }
        if (encPid_ >= 0) { ::kill(encPid_, SIGKILL); ::waitpid(encPid_, nullptr, 0); encPid_ = -1; }
    }
#endif
}

// Spawn `argv` (argv[0] resolved via PATH) with its stdin piped from us. The ffmpeg command line
// is assembled by encoderStart below; this half is pure process plumbing.
static bool spawnEncoderProcess(const char* const argv[]) {
    stopEncoderProcess();
    if (encTestMode_ != EncoderTestMode::Off) {
        encCapturedArgs_.clear();
        for (const char* const* a = argv; *a; a++) {
            if (!encCapturedArgs_.empty()) encCapturedArgs_ += ' ';
            encCapturedArgs_ += *a;
        }
        return encTestMode_ == EncoderTestMode::Record;
    }
#ifdef _WIN32
    // Anonymous pipe, our end non-inheritable; ffmpeg resolved via PATH by CreateProcess.
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE readEnd = nullptr, writeEnd = nullptr;
    if (!CreatePipe(&readEnd, &writeEnd, &sa, 4 * 1024 * 1024)) return false;
    SetHandleInformation(writeEnd, HANDLE_FLAG_INHERIT, 0);
    std::string cmd;
    for (const char* const* a = argv; *a; a++) {
        if (!cmd.empty()) cmd += ' ';
        cmd += '"'; cmd += *a; cmd += '"';
    }
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = readEnd;
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(readEnd);
    if (!ok) { CloseHandle(writeEnd); return false; }
    CloseHandle(pi.hThread);
    encProcess_ = pi.hProcess;
    encStdin_ = writeEnd;
#else
    // posix_spawn, not fork/exec: fork in a threaded process can deadlock on the allocator
    // lock before exec, and a plain exec would leak every parent fd (the HTTP listen socket,
    // the Art-Net/DDP ports) into a child that outlives a restart. Everything except the
    // dup2'd stdin is closed in the child: CLOEXEC_DEFAULT on macOS, closefrom on glibc.
    int fds[2];
    if (::pipe(fds) != 0) return false;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, fds[0], 0);
    pid_t pid = -1;
    int rc;
#ifdef __APPLE__
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_CLOEXEC_DEFAULT);
    rc = ::posix_spawnp(&pid, argv[0], &fa, &attr, const_cast<char* const*>(argv), environ);
    posix_spawnattr_destroy(&attr);
#else
    posix_spawn_file_actions_addclosefrom_np(&fa, 3);
    rc = ::posix_spawnp(&pid, argv[0], &fa, nullptr, const_cast<char* const*>(argv), environ);
#endif
    posix_spawn_file_actions_destroy(&fa);
    ::close(fds[0]);
    if (rc != 0) { ::close(fds[1]); return false; }
    ::signal(SIGPIPE, SIG_IGN);             // a dead ffmpeg surfaces as EPIPE, not a signal
    encPid_ = pid;
    encStdin_ = fds[1];
#endif
    // The writer thread does the BLOCKING writes: the render tick only ever enqueues, so an
    // encoder that stops reading for a while (scheduler starvation under a free-running render
    // loop stalled it >250 ms on the bench) costs queued-then-dropped frames, never a stalled
    // tick, never a torn frame, and never a false death.
    {
        std::lock_guard<std::mutex> lk(encMutex_);   // producers may race this restart
        encWriterStop_ = false;
        encWriterDead_ = false;
        encHead_ = 0;
        encCount_ = 0;
    }
    encWriter_ = std::thread([] {
        for (;;) {
            const std::vector<uint8_t>* frame = nullptr;
            {
                std::unique_lock<std::mutex> lk(encMutex_);
                encCv_.wait(lk, [] { return encWriterStop_ || encCount_ > 0; });
                if (encWriterStop_) return;
                frame = &encSlots_[encHead_];   // the writer owns the head slot until it advances
            }
            size_t off = 0;
            while (off < frame->size()) {
#ifdef _WIN32
                DWORD wrote = 0;
                if (!WriteFile(encStdin_, frame->data() + off,
                               static_cast<DWORD>(frame->size() - off), &wrote, nullptr)) {
                    std::lock_guard<std::mutex> lk(encMutex_);
                    encWriterDead_ = true;
                    return;
                }
                off += wrote;
#else
                const ssize_t n = ::write(encStdin_, frame->data() + off, frame->size() - off);
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0) {
                    std::printf("encoder: pipe closed at %zu/%zu bytes\n", off, frame->size());
                    std::lock_guard<std::mutex> lk(encMutex_);
                    encWriterDead_ = true;   // EPIPE etc.: the process is gone
                    return;
                }
                off += static_cast<size_t>(n);
#endif
            }
            {
                std::lock_guard<std::mutex> lk(encMutex_);
                encHead_ = (encHead_ + 1) % kEncQueueMax;
                encCount_--;
            }
        }
    });
    return true;
}

// The ffmpeg invocation IS the desktop encode contract: raw RGB in at the grid size and rate,
// zerolatency x264 out, 1 s segments on a short rolling playlist (the live tuning that puts
// glass-to-glass at 2-5 s), segments deleted as they fall off it.
bool encoderStart(const EncoderConfig& cfg) {
    char geo[16], rate[8], gop[8], bv[12], out[192];
    std::snprintf(geo, sizeof(geo), "%ux%u", static_cast<unsigned>(cfg.width),
                  static_cast<unsigned>(cfg.height));
    std::snprintf(rate, sizeof(rate), "%u", static_cast<unsigned>(cfg.fps));
    std::snprintf(gop, sizeof(gop), "%u", static_cast<unsigned>(cfg.fps));
    std::snprintf(bv, sizeof(bv), "%uk", static_cast<unsigned>(cfg.bitrateKbit));
    std::snprintf(out, sizeof(out), "%s/stream.m3u8", cfg.outDir);

    // Assembled by index so the x264-only tuning flags stay off other encoders
    // (h264_videotoolbox rejects -tune) without duplicated slots.
    // Size the frame slots HERE, off the render tick: encoderWrite's assign() would otherwise
    // allocate on its first lap, and the tick path must not allocate at all. A frame is
    // width*height*3 (tight RGB, the driver's packing); a failure here fails the start, where
    // the driver already reports it, rather than throwing from a later write.
    const size_t frameBytes = static_cast<size_t>(cfg.width) * cfg.height * 3;
    // Stop FIRST, then resize. The previous writer thread reads a slot's data pointer in its
    // blocking write loop WITHOUT encMutex_ held, so reserving under it is both a data race and,
    // once a geometry or scale change grows frameBytes, a reallocation that frees the buffer the
    // writer is still reading. spawnEncoderProcess stops again below; that call is then a no-op.
    stopEncoderProcess();
    try {
        for (auto& slot : encSlots_) slot.reserve(frameBytes);
    } catch (const std::bad_alloc&) {
        return false;
    }

    const char* encoder = cfg.encoderName ? cfg.encoderName : "libx264";
    const bool x264 = std::strcmp(encoder, "libx264") == 0;
    const char* argv[40];
    size_t i = 0;
    auto add = [&](const char* a) { if (i + 1 < sizeof(argv) / sizeof(argv[0])) argv[i++] = a; };
    for (const char* a : std::initializer_list<const char*>{
             "ffmpeg", "-hide_banner", "-loglevel", "error",
             "-f", "rawvideo", "-pix_fmt", "rgb24", "-s", geo,
             "-r", rate, "-i", "-",
             "-c:v", encoder }) add(a);
    if (x264) { add("-preset"); add("veryfast"); add("-tune"); add("zerolatency"); }
    for (const char* a : std::initializer_list<const char*>{
             "-g", gop, "-b:v", bv,
             "-f", "hls", "-hls_time", "1", "-hls_list_size", "6",
             "-hls_flags", "delete_segments+temp_file", out }) add(a);   // temp_file: the playlist lands by RENAME, never served half-written
    argv[i] = nullptr;
    return spawnEncoderProcess(argv);
}

// ffmpeg writes the playlist and segments to disk itself, so there is nothing in RAM to serve and
// the HTTP server uses its normal file path.
bool hlsSegment(const char*, const uint8_t**, size_t*) { return false; }
void hlsSegmentRelease() {}

int encoderWrite(const uint8_t* data, size_t len) {
    std::lock_guard<std::mutex> lock(encMutex_);
    if (encTestMode_ == EncoderTestMode::Record) {
        if (encTestWriteResult_ != kEncTestWriteNormal) return encTestWriteResult_;
        encCaptured_.emplace_back(data, data + len);
        return static_cast<int>(len);
    }
    if (encWriterDead_) return -1;
    if (encCount_ >= kEncQueueMax) return 0;   // encoder behind: drop-newest, stay live
    // assign() into the reused slot. The capacity was reserved by encoderStart, so this copies
    // without allocating -- including the first lap, which is why the reserve is there.
    encSlots_[(encHead_ + encCount_) % kEncQueueMax].assign(data, data + len);
    encCount_++;
    encCv_.notify_one();
    return static_cast<int>(len);
}

bool encoderRunning() {
    if (encTestMode_ != EncoderTestMode::Off) return encTestMode_ == EncoderTestMode::Record;
    {
        std::lock_guard<std::mutex> lock(encMutex_);
        if (encWriterDead_) return false;
    }
#ifdef _WIN32
    if (!encProcess_) return false;
    return WaitForSingleObject(encProcess_, 0) == WAIT_TIMEOUT;
#else
    if (encPid_ < 0) return false;
    int status = 0;
    const pid_t r = ::waitpid(encPid_, &status, WNOHANG);
    if (r == encPid_) {
        std::printf("encoder: process exited (status %d)\n", status);
        encPid_ = -1;
        return false;
    }
    if (r < 0 && errno == ECHILD) { encPid_ = -1; return false; }   // reaped elsewhere: a stale
                                                                    // pid must never be SIGKILLed
    return true;   // running, or EINTR (a signal is not an exit)
#endif
}

void encoderStop() {
    stopEncoderProcess();
}

void setTestEncoderMode(EncoderTestMode mode) {
    encTestMode_ = mode;
    encTestWriteResult_ = kEncTestWriteNormal;
    if (mode == EncoderTestMode::Off) { encCaptured_.clear(); encCapturedArgs_.clear(); }
}
void setTestEncoderWriteResult(int result) { encTestWriteResult_ = result; }
size_t encoderTestFrameCount() { return encCaptured_.size(); }
size_t encoderTestFrameSize(size_t i) { return i < encCaptured_.size() ? encCaptured_[i].size() : 0; }
const uint8_t* encoderTestFrameData(size_t i) {
    return i < encCaptured_.size() ? encCaptured_[i].data() : nullptr;
}
const char* encoderTestArgs() { return encCapturedArgs_.c_str(); }
void encoderTestClearFrames() { encCaptured_.clear(); }


// --- Raw-interface enumeration (the panel-card `interface` Select) --------------------------
//
// Labels for humans, bind names for ethBindRawInterface, entry 0 always the capture-only row.
// Windows: pcap_findalldevs, labeled by the adapter's friendly description (the pcap device
// name is a GUID nobody recognizes); POSIX: getifaddrs, where the name IS the label.

namespace {
// FIXED storage, refilled in place: a Select's aux keeps pointing at these arrays across
// re-enumerations, so two panel-card instances rebuilding in one sweep can never dangle each
// other's option pointers (rows update under a stale aux, which is harmless; freed rows would
// not be). 16 NICs + the capture row cover any sane host.
constexpr size_t kRawIfMax = 17;
char rawIfLabels_[kRawIfMax][64];
char rawIfNames_[kRawIfMax][64];
const char* rawIfOptions_[kRawIfMax];
size_t rawIfCount_ = 0;
std::vector<std::string> rawIfTest_;   // test seam: label == bind name

void rawIfPush(const char* label, const char* name) {
    if (rawIfCount_ >= kRawIfMax) return;
    // 63 not 64: the Select apply path rejects labels that FILL its 64-byte buffer as
    // overlong, so a row must persist at <= 62 chars or the pick dies on reboot.
    std::snprintf(rawIfLabels_[rawIfCount_], 63, "%s", label);
    std::snprintf(rawIfNames_[rawIfCount_], sizeof(rawIfNames_[0]), "%s", name);
    rawIfCount_++;
}
}  // namespace

void setTestRawInterfaces(const char* const* names, size_t count) {
    // The documented reset is (nullptr, 0), and `names + count` on a null pointer is undefined
    // even when count is zero, so the reset is its own path rather than a degenerate range.
    if (!names || count == 0) { rawIfTest_.clear(); return; }
    rawIfTest_.assign(names, names + count);
}

size_t rawInterfaces(const char* const** optionsOut) {
    rawIfCount_ = 0;
    rawIfPush("none (capture only)", "");
    if (!rawIfTest_.empty()) {
        for (const auto& n : rawIfTest_) rawIfPush(n.c_str(), n.c_str());
    } else {
#ifdef _WIN32
        if (wpcapLoad()) {
            PcapIf* devs = nullptr;
            char err[256] = {};
            if (pcapFindAllDevs_(&devs, err) == 0 && devs) {
                MIB_IF_TABLE2* table = nullptr;
                if (::GetIfTable2(&table) != NO_ERROR) table = nullptr;
                for (const PcapIf* d = devs; d; d = d->next) {
                    // Show only what could carry panel frames. The POSIX branch below does the
                    // same job with a virtual-name blocklist; Windows names tell you nothing, so
                    // the interface table answers it instead. Skipped ONLY when the table was
                    // read: without it every row would be filtered out and the picker would be
                    // an empty dead end on a machine whose NICs are perfectly usable.
                    if (table && !winIsPanelCapableNic(winRowForPcapName(table, d->name))) continue;
                    char desc[256] = {};
                    const char* label = nullptr;
                    if (winDescForPcapName(table, d->name, desc, sizeof(desc))) label = desc;
                    else if (d->description && d->description[0]) label = d->description;
                    else label = d->name;   // pcap reports no description for some adapters
                    char row[64];
                    std::snprintf(row, sizeof(row), "%s", label);
                    // Two identical adapters would collide as Select rows: suffix the device
                    // name's tail so each row stays a distinct, matchable label.
                    for (size_t i = 1; i < rawIfCount_; i++) {
                        if (std::strcmp(rawIfLabels_[i], row) == 0) {
                            const char* tail = d->name + (std::strlen(d->name) > 8 ? std::strlen(d->name) - 8 : 0);
                            std::snprintf(row, sizeof(row), "%s (%s)", label, tail);
                            break;
                        }
                    }
                    rawIfPush(row, d->name);
                }
                if (table) ::FreeMibTable(table);
                pcapFreeAllDevs_(devs);
            }
        }
#else
        // The OS's virtual plumbing can never reach a panel card and only buries the real
        // NICs: loopback plus the well-known virtual prefixes (macOS: VPN tunnels, the
        // AirDrop/AirPlay radios, Apple-silicon debug, bridges; Linux: container veths).
        static constexpr const char* kVirtualPrefixes[] = {
            "lo", "utun", "awdl", "llw", "anpi", "bridge", "gif", "stf", "ap", "pktap",
            "veth", "docker", "br-", "virbr",
        };
            // The adapter's negotiated link speed in Mbit, or 0 when the OS will not state one
            // (a virtual interface, a link that is down, Wi-Fi on macOS which reports only
            // "autoselect"). Rides in the label for the same reason as the Windows branch: the
            // name alone does not say which entry is the 1 Gb NIC and which is a tunnel.
            auto linkMbps = [](const char* ifname) -> unsigned {
#ifdef __linux__
                // sysfs states it directly, in Mbit. Absent or -1 for a virtual or down link.
                char path[128];
                std::snprintf(path, sizeof(path), "/sys/class/net/%s/speed", ifname);
                FILE* f = std::fopen(path, "r");
                if (!f) return 0;
                long v = 0;
                const bool ok = std::fscanf(f, "%ld", &v) == 1;
                std::fclose(f);
                return (ok && v > 0) ? static_cast<unsigned>(v) : 0;
#elif defined(__APPLE__)
                // macOS has no speed field: the negotiated rate is encoded as the media
                // SUBTYPE, so map the ones that name a rate. Wi-Fi and "autoselect" report no
                // subtype we can turn into a number, which is exactly when 0 is the honest
                // answer rather than a guess.
                const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
                if (fd < 0) return 0;
                ifmediareq req{};
                std::snprintf(req.ifm_name, sizeof(req.ifm_name), "%s", ifname);
                unsigned mbps = 0;
                if (::ioctl(fd, SIOCGIFMEDIA, &req) == 0 && (req.ifm_status & IFM_ACTIVE)) {
                    switch (IFM_SUBTYPE(req.ifm_active)) {
                        case IFM_10_T:   mbps = 10;    break;
                        case IFM_100_TX: mbps = 100;   break;
                        case IFM_1000_T: mbps = 1000;  break;
                        case IFM_2500_T: mbps = 2500;  break;
                        case IFM_5000_T: mbps = 5000;  break;
                        case IFM_10G_T:  mbps = 10000; break;
                        default: break;
                    }
                }
                ::close(fd);
                return mbps;
#else
                (void)ifname;
                return 0;
#endif
            };

        auto isVirtual = [](const char* n) {
            for (const char* p : kVirtualPrefixes) {
                const size_t l = std::strlen(p);
                if (std::strncmp(n, p, l) == 0 && (n[l] == 0 || (n[l] >= '0' && n[l] <= '9')))
                    return true;
            }
            return false;
        };
        ifaddrs* addrs = nullptr;
        if (::getifaddrs(&addrs) == 0 && addrs) {
            for (const ifaddrs* a = addrs; a; a = a->ifa_next) {
                if (!a->ifa_name || !(a->ifa_flags & IFF_UP)) continue;
                if ((a->ifa_flags & IFF_LOOPBACK) || isVirtual(a->ifa_name)) continue;
                bool seen = false;
                for (size_t i = 1; i < rawIfCount_; i++)
                    if (std::strcmp(rawIfNames_[i], a->ifa_name) == 0) { seen = true; break; }
                if (seen) continue;   // getifaddrs lists one row per address family
                // Label carries the speed, bind name does not: the name is the adapter's
                // identity and the speed changes when a link renegotiates (platform.h § raw
                // interfaces). Same "NAME, N Gb" shape as the Windows branch.
                char label[64];
                std::snprintf(label, sizeof(label), "%s", a->ifa_name);
                appendLinkSpeed(label, sizeof(label), linkMbps(a->ifa_name));
                rawIfPush(label, a->ifa_name);
            }
            ::freeifaddrs(addrs);
        }
#endif
    }
    for (size_t i = 0; i < rawIfCount_; i++) rawIfOptions_[i] = rawIfLabels_[i];
    if (optionsOut) *optionsOut = rawIfOptions_;
    return rawIfCount_;
}

const char* rawInterfaceName(size_t i) {
    if (i == 0 || i >= rawIfCount_) return nullptr;   // row 0: capture only
    return rawIfNames_[i];
}

} // namespace mm::platform
