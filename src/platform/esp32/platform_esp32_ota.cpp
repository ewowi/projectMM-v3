// OTA — fetch firmware from a URL and flash it to the next OTA partition.
//
// Cut out of platform_esp32.cpp (plan-23) for size + readability. The
// file owns the OtaTaskParams + otaTask shape in an anonymous namespace;
// the rest of the platform layer talks to it only through the public
// mm::platform::http_fetch_to_ota symbol declared in platform.h. Move
// was a code-organization change with no API delta.

#include "platform/platform.h"

#include "core/FirmwareImage.h"  // identify/moonBaseRejection: vetting a MoonBase image

#include "esp_https_ota.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"   // esp_partition_find_first/erase_range/write: MoonBase
#include "esp_image_format.h" // esp_image_verify + the image/segment headers
#include "sdkconfig.h"     // CONFIG_IDF_FIRMWARE_CHIP_ID: which chip THIS build is for
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_system.h"
#include "esp_heap_caps.h"   // heap_caps_malloc/free — the upload chunk buffer
#include "esp_log.h"
#include "nvs.h"           // moonbaseStageInstallUrl: the URL handoff to MoonBase

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>        // unique_ptr — frees the upload buffer on every exit path
#include <new>           // std::nothrow for the OtaTaskParams alloc below

namespace mm::platform {

// One upload chunk. 4 KB matches the flash page granularity esp_ota_write prefers and is
// the size the HTTP path already streams in.
constexpr size_t kOtaChunkBytes = 4096;

// The factory partition, which holds MoonBase, or null on the dual-OTA tables. One lookup for
// every caller below: the same find_first was written out four times as this file grew.
const esp_partition_t* moonBasePartition() {
    return esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                    ESP_PARTITION_SUBTYPE_APP_FACTORY, nullptr);
}

namespace {

// Heap-allocated task parameters. Task owns this and frees it on exit.
struct OtaTaskParams {
    char url[512];
    char* statusBuf;
    size_t statusBufLen;
    uint32_t* bytesReadOut;   // current bytes downloaded
    uint32_t* bytesTotalOut;  // image size; 0 until esp_https_ota reports it
};

// Write to a status buffer with bounded length. snprintf truncates safely.
//
// ONE writer for the file. Three call sites had grown an identical `setStatus` lambda of their
// own, which also put the format string behind a variadic template: correct, since every call
// passes a literal, but not PROVABLE, and CodeQL flagged it (cpp/non-constant-format). A plain
// varargs function takes the printf format attribute, so the compiler now checks each format
// against its arguments and a non-literal one is a build error rather than a scanner note.
__attribute__((format(printf, 3, 4)))
void statusf(char* buf, size_t len, const char* fmt, ...) {
    if (!buf || len == 0) return;
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, len, fmt, args);
    va_end(args);
}

void otaSetStatus(OtaTaskParams* p, const char* fmt, ...) {
    if (!p->statusBuf || p->statusBufLen == 0) return;
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(p->statusBuf, p->statusBufLen, fmt, args);
    va_end(args);
}

void otaTask(void* arg) {
    auto* p = static_cast<OtaTaskParams*>(arg);

    otaSetStatus(p, "downloading");
    *p->bytesReadOut = 0;
    *p->bytesTotalOut = 0;   // unknown until esp_https_ota reports it

    // `esp_crt_bundle_attach` enables the bundled-trust-anchor mode for TLS verification — the same
    // mechanism Chrome/curl use for general HTTPS (api.github.com, objects.githubusercontent.com, …).
    // No baked cert. It's attached unconditionally: for an https URL it verifies the server; for a
    // plain-http LAN OTA (MoonDeck serving a local build) it goes unused, but its presence satisfies
    // esp_https_ota_begin's "server verification enabled" check, so the fetch proceeds over plain TCP.
    esp_http_client_config_t http_config = {};
    http_config.url = p->url;
    http_config.crt_bundle_attach = esp_crt_bundle_attach;
    http_config.timeout_ms = 10000;
    // GitHub release-asset URLs 302-redirect to objects.githubusercontent.com.
    // Default redirect handling is off in esp_http_client; force-follow.
    http_config.disable_auto_redirect = false;
    http_config.max_redirection_count = 10;
    // ESP-IDF's default HTTP header buffer is 512 bytes per direction. GitHub's
    // 302 redirect response includes a multi-KB `content-security-policy`
    // header that overflows it ("HTTP_CLIENT: Out of buffer") and the OTA
    // fails before the .bin download even starts. Raising both sides to 4 KB
    // covers GitHub's longest headers with room to spare; the cost is ~7 KB
    // of heap during the OTA fetch, freed when the OTA task exits.
    http_config.buffer_size = 4096;
    http_config.buffer_size_tx = 4096;

    esp_https_ota_config_t ota_config = {};
    ota_config.http_config = &http_config;
    // Performs partial-image-write + commit + boot-pointer flip internally.

    esp_https_ota_handle_t handle = nullptr;
    esp_err_t err = esp_https_ota_begin(&ota_config, &handle);
    if (err != ESP_OK) {
        // esp_https_ota_begin collapses ~6 distinct failures (DNS, TLS,
        // HTTP, partition init, header-buffer overflow) into one ESP_FAIL,
        // so the only useful detail is in the IDF log on the serial console.
        // We surface the IDF error name plus a pointer to the log.
        otaSetStatus(p, "error: ota begin %s (see serial log)",
                     esp_err_to_name(err));
        delete p;
        vTaskDelete(nullptr);
        return;
    }

    // NOT A MOONBASE IMAGE. The mirror of the check the MoonBase install runs, and it exists for
    // the same reason: the two images sit side by side on the releases page and are one paste
    // apart. Writing MoonBase into the APP slot is the worse direction, because both partitions
    // then hold MoonBase and every route out resolves to the partition it is running from
    // (ESP_ERR_OTA_PARTITION_CONFLICT, 0x1501): the device answers, serves a page, and cannot be
    // recovered without a cable. Found on the bench by installing exactly that.
    //
    // Read from the incoming image before a byte is committed, so a wrong file costs nothing.
    esp_app_desc_t incoming = {};
    if (esp_https_ota_get_img_desc(handle, &incoming) == ESP_OK &&
        std::strncmp(incoming.project_name, "projectMM-moonbase",
                     sizeof(incoming.project_name)) == 0) {
        otaSetStatus(p, "error: that is a MoonBase image, not an app");
        esp_https_ota_abort(handle);
        delete p;
        vTaskDelete(nullptr);
        return;
    }

    int total = esp_https_ota_get_image_size(handle);
    if (total > 0) {
        // Publish the real total so the UI can render "X KB / Y KB".
        // FirmwareUpdateModule's tick1s() rebuildControls picks this up on
        // the next 1 Hz poll (re-binds the progress descriptor with the new
        // total snapshot).
        *p->bytesTotalOut = static_cast<uint32_t>(total);
    }
    otaSetStatus(p, "flashing");

    while ((err = esp_https_ota_perform(handle)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
        int got = esp_https_ota_get_image_len_read(handle);
        if (got >= 0) *p->bytesReadOut = static_cast<uint32_t>(got);
        // The counts ride the STATUS, the same "N of M bytes" shape MoonBase's own page reports
        // and the MoonBase write uses. One channel, so the UI reads progress the same way
        // whichever image is being installed: these numbers used to reach a progress CONTROL
        // that no longer exists, so an app install showed an indeterminate sweep that never
        // resolved, on a device where it was working perfectly.
        if (total > 0) {
            otaSetStatus(p, "flashing: %u of %u bytes",
                         static_cast<unsigned>(got > 0 ? got : 0), static_cast<unsigned>(total));
        }
    }
    if (err != ESP_OK) {
        otaSetStatus(p, "error: ota perform %s", esp_err_to_name(err));
        esp_https_ota_abort(handle);
        delete p;
        vTaskDelete(nullptr);
        return;
    }

    if (!esp_https_ota_is_complete_data_received(handle)) {
        otaSetStatus(p, "error: incomplete download");
        esp_https_ota_abort(handle);
        delete p;
        vTaskDelete(nullptr);
        return;
    }

    err = esp_https_ota_finish(handle);
    if (err != ESP_OK) {
        // After finish, abort isn't valid — handle is consumed. Surface and exit.
        otaSetStatus(p, "error: ota finish %s", esp_err_to_name(err));
        delete p;
        vTaskDelete(nullptr);
        return;
    }

    // Final byte count match — pull from the OTA handle one last time so the
    // UI's last frame before reboot shows a clean "Y KB / Y KB".
    if (*p->bytesTotalOut > 0) *p->bytesReadOut = *p->bytesTotalOut;
    otaSetStatus(p, "rebooting");
    delete p;
    // 600 ms delay gives the HTTP response time to make it back to the browser
    // before the device drops the socket on restart.
    vTaskDelay(pdMS_TO_TICKS(600));
    esp_restart();
}

}  // anonymous namespace

bool http_fetch_to_ota(const char* url,
                       char* statusBuf, size_t statusBufLen,
                       uint32_t* bytesReadOut, uint32_t* bytesTotalOut) {
    if (!url || !statusBuf || statusBufLen == 0 || !bytesReadOut || !bytesTotalOut) {
        return false;
    }

    // Reject oversize URLs explicitly rather than silently truncating with
    // strncpy — a truncated URL almost always 404s or fetches the wrong
    // file, with no clue in the status surface.
    size_t urlLen = std::strlen(url);
    constexpr size_t kUrlMax = sizeof(OtaTaskParams::url) - 1;
    if (urlLen > kUrlMax) {
        std::snprintf(statusBuf, statusBufLen,
                      "error: url too long (%zu > %zu)", urlLen, kUrlMax);
        return false;
    }

    // std::nothrow so OOM doesn't abort the process. Status string carries
    // the failure back to the route, which returns 500 to the browser.
    auto* p = new (std::nothrow) OtaTaskParams{};
    if (!p) {
        std::snprintf(statusBuf, statusBufLen, "error: out of memory");
        return false;
    }
    std::memcpy(p->url, url, urlLen + 1);   // includes NUL; size already verified
    p->statusBuf = statusBuf;
    p->statusBufLen = statusBufLen;
    p->bytesReadOut = bytesReadOut;
    p->bytesTotalOut = bytesTotalOut;

    // 12 KB stack matches v1's working number (TLS handshake + HTTPS body
    // buffering inside esp_https_ota). Priority 5 = above idle, below
    // FreeRTOS critical drivers.
    BaseType_t ok = xTaskCreate(&otaTask, "urlOta", 12288, p, 5, nullptr);
    if (ok != pdPASS) {
        otaSetStatus(p, "error: task create failed");
        delete p;
        return false;
    }
    return true;
}


bool otaWriteStream(FsWriteSrc src, void* user, size_t contentLen,
                    char* statusBuf, size_t statusBufLen, uint32_t* bytesReadOut) {
    if (!src || !statusBuf || statusBufLen == 0 || !bytesReadOut) return false;
    // Names the buffer once; the formatting itself is statusf's, which the compiler format-checks.
    auto setStatus = [&](const char* fmt, auto... a) { statusf(statusBuf, statusBufLen, fmt, a...); };

    const esp_partition_t* part = esp_ota_get_next_update_partition(nullptr);
    if (!part) { setStatus("error: no OTA partition"); return false; }

    // SINGLE-SLOT GUARD. esp_ota_get_next_update_partition iterates only OTA subtypes and falls
    // back to the FIRST ota slot it finds, so on a table with one ota_0 (the MoonBase layout) it
    // hands back the partition we are executing from. Erasing that is a brick mid-flash. IDF also
    // refuses it inside esp_ota_begin (ESP_ERR_OTA_PARTITION_CONFLICT), but failing here says WHY
    // and names the fix. On a dual-OTA table this never fires.
    if (part == esp_ota_get_running_partition()) {
        setStatus("error: one app slot, reboot to MoonBase first");
        return false;
    }
    // SIZE GUARD. Without it an oversized image fails partway through esp_ota_write, leaving the
    // target slot half-written and the user staring at a generic write error. Content-Length is
    // advisory, so this only fires when the caller knows the size.
    if (contentLen && contentLen > part->size) {
        setStatus("error: image too large (%u > %u)",
                  static_cast<unsigned>(contentLen), static_cast<unsigned>(part->size));
        return false;
    }

    setStatus("flashing");
    esp_ota_handle_t handle = 0;
    // OTA_SIZE_UNKNOWN: the upload streams, so we don't pre-declare the exact size (Content-Length
    // is advisory for the UI); esp_ota_begin erases lazily as writes arrive.
    esp_err_t err = esp_ota_begin(part, OTA_SIZE_UNKNOWN, &handle);
    if (err != ESP_OK) { setStatus("error: ota begin %s", esp_err_to_name(err)); return false; }

    // Pull the upload body chunk-by-chunk and write each into the partition — the same producer
    // callback fsWriteStream drives, here feeding esp_ota_write instead of a file. `abort` from the
    // caller (an incomplete/timed-out upload) fails the OTA, and esp_ota_abort discards the partial.
    // Heap, not static, and not the stack either. 4 KB is far too large for a task frame, but as a
    // `static` it held 4 KB of INTERNAL RAM from boot to power-off for a buffer used only while a
    // firmware image is uploading — minutes of the device's life at most, and never at all on a
    // device that is never updated. An OTA is the one moment when spare RAM is least scarce (the
    // render path is the only other big consumer), so allocating here and freeing at every exit
    // costs nothing and gives the 4 KB back to WiFi and the HTTP stack for the other 99.9% of
    // uptime. Surfaced by check_footprint's STATIC column: this file read 1016 B of code against
    // 4096 B of static.
    //
    // Failing the alloc aborts the OTA cleanly rather than proceeding — a firmware write with no
    // buffer is not something to degrade around.
    // unique_ptr, not a raw malloc: there are six exit paths below (abort, write error, truncated
    // upload, three esp_ota failures) and a leak on any of them would be a 4 KB hole per attempt.
    const std::unique_ptr<uint8_t, decltype(&heap_caps_free)> owned(
        static_cast<uint8_t*>(heap_caps_malloc(kOtaChunkBytes, MALLOC_CAP_8BIT)), &heap_caps_free);
    uint8_t* const buf = owned.get();
    if (!buf) {
        setStatus("error: out of memory for the upload buffer");
        esp_ota_abort(handle);
        return false;
    }
    uint32_t written = 0;
    bool vetted = false;
    for (;;) {
        bool abort = false;
        const size_t n = src(reinterpret_cast<char*>(buf), kOtaChunkBytes, user, &abort);
        if (abort) {
            setStatus("error: upload aborted");
            esp_ota_abort(handle);
            return false;
        }
        if (n == 0) break;   // clean EOF — whole body delivered
        // NOT A MOONBASE IMAGE. The mirror of the MoonBase install's check: writing MoonBase into
        // the APP slot leaves both partitions holding it, and every route out then resolves to the
        // partition being run from, so the device answers and cannot be recovered without a cable.
        //
        // Decided on ENOUGH BYTES, not on the first chunk. A producer may deliver fewer than the
        // descriptor needs (a slow socket hands back what it has), and identifying a short prefix
        // reports "no description" for an image that has one: the guard would pass and the wrong
        // image would land. So hold the prefix back until there is enough to read.
        if (!vetted) {
            if (written + n < firmware::kIdentifyBytes) {
                // Not enough yet, and nothing written: keep accumulating in the OTA partition is
                // not an option (a rejected image must leave no bytes), so refuse a body that
                // ends before it can be identified. Any real image is far larger.
                if (contentLen && contentLen < firmware::kIdentifyBytes) {
                    setStatus("error: too short to be a firmware image");
                    esp_ota_abort(handle);
                    return false;
                }
            }
            const auto info = firmware::identify(buf, n);
            if (info.valid && !info.described && n < firmware::kIdentifyBytes) {
                setStatus("error: could not identify the image");
                esp_ota_abort(handle);
                return false;
            }
            vetted = true;
            if (info.described &&
                std::strcmp(info.project, "projectMM-moonbase") == 0) {
                setStatus("error: that is a MoonBase image, not an app");
                esp_ota_abort(handle);
                return false;
            }
        }
        err = esp_ota_write(handle, buf, n);
        if (err != ESP_OK) {
            setStatus("error: ota write %s", esp_err_to_name(err));
            esp_ota_abort(handle);
            return false;
        }
        written += static_cast<uint32_t>(n);
        *bytesReadOut = written;
    }
    // Guard a truncated upload: if the client sent fewer bytes than Content-Length, the image is
    // incomplete — don't commit a half-image. (contentLen 0 = unknown; skip the check then.)
    if (contentLen && written < contentLen) {
        setStatus("error: incomplete upload (%u/%u)",
                  static_cast<unsigned>(written), static_cast<unsigned>(contentLen));
        esp_ota_abort(handle);
        return false;
    }

    err = esp_ota_end(handle);   // validates the image (magic/checksum); consumes the handle
    if (err != ESP_OK) { setStatus("error: ota end %s", esp_err_to_name(err)); return false; }
    err = esp_ota_set_boot_partition(part);
    if (err != ESP_OK) { setStatus("error: set boot %s", esp_err_to_name(err)); return false; }

    setStatus("rebooting");
    // Image committed + boot pointer flipped. Return to the caller so it can send its HTTP 200
    // BEFORE the reboot (the caller closes the socket + reboots, same sequence as /api/reboot) —
    // that's what lets the browser see a clean "flashed" response instead of an aborted socket.
    return true;
}

// ---- Updating MOONBASE ITSELF -------------------------------------------------------------
//
// The app installs MoonBase, the mirror of MoonBase installing the app. Nothing else can: a
// board cannot rewrite the partition it is executing from, so the factory slot is writable only
// while the app is running, and the app slot only while MoonBase is. Without this path a device
// whose recovery image is broken needs a cable, which is the one failure MoonBase exists to
// prevent. A bench S3 hit it: its MoonBase could not complete a download, so it could not
// install the app that would have replaced it.
//
// esp_ota_* CANNOT be used here. It targets OTA subtypes only and refuses a factory partition,
// so this writes raw flash, and with that loses the validation esp_ota_end performs. The checks
// below replace it, and their ORDER is the safety property: everything that can reject an image
// is done from the FIRST CHUNK, before a single byte is erased. Point this at an HTML error
// page, the wrong chip's image, or an app firmware, and the device still has its MoonBase.

// Does this first chunk begin a MoonBase image for THIS chip? The parsing and the rules live in
// core/FirmwareImage.h so a host test can drive them: this code erases a device's only recovery
// image, and "the check was never exercised" is not a risk worth carrying for a header parse.
bool moonBaseImageRejected(const uint8_t* buf, size_t n, char* why, size_t whyLen) {
    const auto info = firmware::identify(buf, n);
    const char* reason = firmware::moonBaseRejection(
        info, static_cast<firmware::ChipId>(CONFIG_IDF_FIRMWARE_CHIP_ID));
    if (!reason) return false;
    std::snprintf(why, whyLen, "error: %s", reason);
    return true;
}

bool otaWriteMoonBase(FsWriteSrc src, void* user, size_t contentLen,
                      char* statusBuf, size_t statusBufLen, uint32_t* bytesReadOut) {
    if (!src || !statusBuf || statusBufLen == 0 || !bytesReadOut) return false;
    // Names the buffer once; the formatting itself is statusf's, which the compiler format-checks.
    auto setStatus = [&](const char* fmt, auto... a) { statusf(statusBuf, statusBufLen, fmt, a...); };

    const esp_partition_t* part = moonBasePartition();
    if (!part) { setStatus("error: no MoonBase on this device"); return false; }
    // The inverse of otaWriteStream's single-slot guard, and the reason this path is safe at all:
    // the app runs from ota_0, so factory is never what we are executing. Running from MoonBase
    // means the app slot is the writable one, and this is the wrong call.
    if (part == esp_ota_get_running_partition()) {
        setStatus("error: running from MoonBase, boot the app first");
        return false;
    }
    if (contentLen && contentLen > part->size) {
        setStatus("error: image too large (%u > %u)",
                  static_cast<unsigned>(contentLen), static_cast<unsigned>(part->size));
        return false;
    }

    const std::unique_ptr<uint8_t, decltype(&heap_caps_free)> owned(
        static_cast<uint8_t*>(heap_caps_malloc(kOtaChunkBytes, MALLOC_CAP_8BIT)), &heap_caps_free);
    uint8_t* const buf = owned.get();
    if (!buf) { setStatus("error: out of memory for the update buffer"); return false; }

    setStatus("checking");
    // FIRST CHUNK BEFORE ANY ERASE. Everything that can reject the image is decided here, while
    // the existing MoonBase is still intact and the worst case is a status line.
    bool abort = false;
    size_t first = src(reinterpret_cast<char*>(buf), kOtaChunkBytes, user, &abort);
    if (abort || first == 0) { setStatus("error: no image received"); return false; }
    if (moonBaseImageRejected(buf, first, statusBuf, statusBufLen)) return false;

    // PAST THIS LINE THE DEVICE HAS NO RECOVERY IMAGE until the write completes. Erase and write
    // in one pass: on a 4 MB board there is nowhere to stage 743 KB first (the app slot has
    // ~520 KB free, the filesystem 548), so a second copy is not an option the hardware offers.
    setStatus("erasing");
    esp_err_t err = esp_partition_erase_range(part, 0, part->size);
    if (err != ESP_OK) { setStatus("error: erase %s", esp_err_to_name(err)); return false; }

    setStatus("writing MoonBase");
    uint32_t written = 0;     // bytes committed to flash
    size_t   held    = first; // bytes sitting in buf, not yet written
    bool     eof     = false;
    for (;;) {
        // esp_partition_write needs a 4-byte-aligned LENGTH, which esp_ota_write used to absorb.
        //
        // So write only the whole 4-byte groups currently held, and KEEP the remainder in the
        // buffer for the next read to extend. Padding a short chunk instead looks right and is
        // not: a chunk arrives short whenever the producer has less to give, which on the URL
        // path is any TLS read rather than only the last one. The padding then reaches flash,
        // the offset advances by the unpadded count, and the next write lands up to 3 bytes back
        // over it, corrupting the image at every short read. esp_image_verify catches the result,
        // but only after the slot is erased, which is the failure this design exists to avoid.
        //
        // At EOF the remainder IS the image's last bytes, so it is padded to a word with 0xFF
        // (what erased flash reads as) and written. Only there is padding correct.
        const size_t whole = eof ? ((held + 3u) & ~size_t{3}) : (held & ~size_t{3});
        if (whole) {
            if (eof && whole > held) std::memset(buf + held, 0xFF, whole - held);
            // Compared on the IMAGE bytes, not the padded write: at EOF `whole` rounds up past
            // the image's end, and rejecting a slot-filling image for its own padding would be
            // refusing something that fits. Unreachable at today's sizes; correct anyway.
            if (written + (eof ? held : whole) > part->size) {
                setStatus("error: image overruns the slot");
                return false;
            }
            err = esp_partition_write(part, written, buf, whole);
            if (err != ESP_OK) { setStatus("error: write %s", esp_err_to_name(err)); return false; }
            // The IMAGE grew by what it held, not by the padding: a padded tail is flash the
            // image does not occupy, and counting it would report more written than was sent.
            written += static_cast<uint32_t>(eof ? held : whole);
            *bytesReadOut = written;
            // The counts ride the STATUS, the way MoonBase's own page reports them: one channel
            // for the UI to read, and the overlay draws its bar from a string it already polls.
            if (contentLen) {
                setStatus("writing MoonBase: %u of %u bytes",
                          static_cast<unsigned>(written), static_cast<unsigned>(contentLen));
            }
            held -= eof ? held : whole;
        }
        if (eof) break;

        // Keep the unwritten remainder at the front, then read after it.
        if (held) std::memmove(buf, buf + (whole ? whole : 0), held);
        abort = false;
        const size_t got = src(reinterpret_cast<char*>(buf) + held, kOtaChunkBytes - held,
                               user, &abort);
        if (abort) { setStatus("error: transfer aborted, MoonBase is incomplete"); return false; }
        if (got == 0) eof = true;      // one more pass, to flush what is held
        held += got;
    }
    if (contentLen && written < contentLen) {
        setStatus("error: incomplete transfer (%u/%u), MoonBase is incomplete",
                  static_cast<unsigned>(written), static_cast<unsigned>(contentLen));
        return false;
    }

    // What esp_ota_end did on the app path: checksum the image actually on flash. A failure here
    // is the serious one, since the slot no longer holds a working MoonBase, so it says so
    // plainly rather than reporting a generic write error.
    esp_partition_pos_t pos = { .offset = part->address, .size = part->size };
    esp_image_metadata_t meta = {};
    if (esp_image_verify(ESP_IMAGE_VERIFY, &pos, &meta) != ESP_OK) {
        setStatus("error: MoonBase did not verify, retry before rebooting");
        return false;
    }
    // No reboot and no boot-partition change: the app keeps running, and the new MoonBase is
    // simply what the device falls back to from now on.
    setStatus("idle");
    return true;
}

// Pull a MoonBase image from a URL, feeding the same writer the upload path uses.
//
// esp_https_ota cannot serve this: it targets OTA subtypes only and picks the partition itself,
// so the download is driven by hand and its bytes handed to otaWriteMoonBase through the same
// producer callback an upload uses. One writer, one set of checks, two sources.
struct UrlPull {
    esp_http_client_handle_t client;
    bool failed;
};

size_t urlPullChunk(char* out, size_t cap, void* user, bool* abort) {
    auto* u = static_cast<UrlPull*>(user);
    const int n = esp_http_client_read(u->client, out, static_cast<int>(cap));
    if (n < 0) { u->failed = true; *abort = true; return 0; }
    return static_cast<size_t>(n);   // 0 is a clean end of body
}

bool moonBaseFetchUrlSync(const char* url, char* statusBuf, size_t statusBufLen,
                          uint32_t* bytesReadOut, uint32_t* bytesTotalOut) {
    // Names the buffer once; the formatting itself is statusf's, which the compiler format-checks.
    auto setStatus = [&](const char* fmt, auto... a) { statusf(statusBuf, statusBufLen, fmt, a...); };
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    // Same TLS and redirect handling the app's OTA fetch needs, and for the same reasons: a
    // release asset 302s to objects.githubusercontent.com, whose headers overflow the 512-byte
    // default buffer.
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms = 10000;
    cfg.disable_auto_redirect = false;
    cfg.max_redirection_count = 10;
    cfg.buffer_size = 4096;
    cfg.buffer_size_tx = 4096;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) { setStatus("error: cannot reach that URL"); return false; }
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        setStatus("error: cannot start the download (%s)", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return false;
    }
    const int64_t len = esp_http_client_fetch_headers(client);
    const int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        // A 404 page is a valid HTTP response carrying HTML, which the image checks would catch
        // anyway; failing here says the useful thing instead of "not a firmware image".
        setStatus("error: the server answered %d", status);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }
    if (bytesTotalOut) *bytesTotalOut = len > 0 ? static_cast<uint32_t>(len) : 0;

    UrlPull pull = { client, false };
    const bool ok = otaWriteMoonBase(&urlPullChunk, &pull, len > 0 ? static_cast<size_t>(len) : 0,
                                     statusBuf, statusBufLen, bytesReadOut);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (!ok && pull.failed) setStatus("error: the download was interrupted");
    return ok;
}

// The install runs on its own task so the HTTP request can answer 202 immediately, exactly as the
// app's URL install does. That is not a detail: while the request is open the browser cannot poll
// for progress, so a synchronous install can only ever report "installing" and then "installed".
// Same task shape, same status buffer, same byte counters, so ONE progress display serves both.
void moonBaseUrlTask(void* arg) {
    auto* p = static_cast<OtaTaskParams*>(arg);
    moonBaseFetchUrlSync(p->url, p->statusBuf, p->statusBufLen, p->bytesReadOut, p->bytesTotalOut);
    // No reboot on either outcome: the app is running from the other partition and is untouched.
    // On failure the status already says what went wrong, and it stays there for the UI to read.
    delete p;
    vTaskDelete(nullptr);
}

bool otaFetchMoonBaseUrl(const char* url, char* statusBuf, size_t statusBufLen,
                         uint32_t* bytesReadOut, uint32_t* bytesTotalOut) {
    if (!url || !statusBuf || statusBufLen == 0 || !bytesReadOut || !bytesTotalOut) return false;
    const size_t urlLen = std::strlen(url);
    auto* p = new (std::nothrow) OtaTaskParams{};
    if (!p) {
        std::snprintf(statusBuf, statusBufLen, "error: out of memory");
        return false;
    }
    if (urlLen >= sizeof(p->url)) {
        std::snprintf(statusBuf, statusBufLen, "error: url too long");
        delete p;
        return false;
    }
    std::memcpy(p->url, url, urlLen + 1);
    p->statusBuf = statusBuf;
    p->statusBufLen = statusBufLen;
    p->bytesReadOut = bytesReadOut;
    p->bytesTotalOut = bytesTotalOut;
    *bytesReadOut = 0;
    *bytesTotalOut = 0;
    std::snprintf(statusBuf, statusBufLen, "starting");
    // 8 KB against the app OTA's 12: no esp_https_ota state machine here, just an http client
    // feeding a writer. Priority 5 and the same core assignment as urlOta.
    if (xTaskCreate(&moonBaseUrlTask, "mbOta", 8192, p, 5, nullptr) != pdPASS) {
        std::snprintf(statusBuf, statusBufLen, "error: task create failed");
        delete p;
        return false;
    }
    return true;
}


// Does this device's partition table carry a factory app? True on the MoonBase layout, false on
// the dual-OTA tables. The caller uses it to decide whether an update needs the
// reboot-into-MoonBase hop, and the UI to say which kind of device this is.
bool otaHasMoonBase() {
    return moonBasePartition() != nullptr;
}

// Which MoonBase does this device carry? Read from the factory partition's app descriptor, the
// struct IDF puts at a fixed offset in every image, so this costs one flash read and needs no
// reboot: the app can report the recovery image's version while running normally.
//
// MoonBase answers the same question about ITSELF at GET /api/version (moonbase_main.cpp), which
// is a different question and not a second copy of this one: that route reports the image it is
// EXECUTING (esp_app_get_description, no flash read), this one reports an image it is not.
//
// The value is PROJECT_VER, set by build_moonbase() to the same string the app burns into
// MM_VERSION, which is what lets the caller compare the two by equality.
bool otaMoonBaseVersion(char* out, size_t len) {
    if (!out || len == 0) return false;
    out[0] = 0;
    const esp_partition_t* part = moonBasePartition();
    if (!part) return false;
    esp_app_desc_t desc = {};
    if (esp_ota_get_partition_description(part, &desc) != ESP_OK) return false;
    std::snprintf(out, len, "%.*s", static_cast<int>(sizeof(desc.version)), desc.version);
    return out[0] != 0;
}

// When MoonBase was built, from the same descriptor. The UI shows the app's build stamp beside its
// version, and shows the same pair for MoonBase, so this is the other half of that.
bool otaMoonBaseBuild(char* out, size_t len) {
    if (!out || len == 0) return false;
    out[0] = 0;
    const esp_partition_t* part = moonBasePartition();
    if (!part) return false;
    esp_app_desc_t desc = {};
    if (esp_ota_get_partition_description(part, &desc) != ESP_OK) return false;
    std::snprintf(out, len, "%.*s %.*s",
                  static_cast<int>(sizeof(desc.date)), desc.date,
                  static_cast<int>(sizeof(desc.time)), desc.time);
    return out[0] != 0;
}

// The factory slot's size, for the UI to show beside the app's partition figure.
//
// The slot rather than the image: esp_image_get_metadata reads through the bootloader's flash
// mapping, which is arranged for the RUNNING partition, and on the bench it simply errored for
// the factory one, leaving the row absent. The image's own length is not worth a second mechanism
// to obtain, since what a user wants from this row is whether the slot has room, and the
// descriptor read above already proves an image is there.
bool otaMoonBaseSize(uint32_t* used, uint32_t* total) {
    const esp_partition_t* part = moonBasePartition();
    if (!part) return false;
    if (total) *total = part->size;
    if (used) {
        // Read the image length straight out of the header, which is a plain partition read and
        // needs no flash mapping: the first bytes of a valid image are its header, and its
        // segments follow. esp_partition_read is the same call the vetting path uses.
        esp_image_header_t hdr = {};
        *used = 0;
        if (esp_partition_read(part, 0, &hdr, sizeof(hdr)) == ESP_OK &&
            hdr.magic == ESP_IMAGE_HEADER_MAGIC) {
            // Walk the segment table to the end of the last segment: that is the image length.
            uint32_t off = sizeof(esp_image_header_t);
            for (uint8_t i = 0; i < hdr.segment_count && off < part->size; i++) {
                esp_image_segment_header_t seg = {};
                if (esp_partition_read(part, off, &seg, sizeof(seg)) != ESP_OK) { off = 0; break; }
                if (seg.data_len > part->size) { off = 0; break; }   // a corrupt length
                off += sizeof(seg) + seg.data_len;
            }
            // The segments only: the padding, 1-byte checksum and 32-byte hash the image ends
            // with are not counted, so this reads ~48 bytes under the file on disk. That is
            // deliberate rather than missed. This figure answers "how full is the slot", where
            // 48 bytes in 896 KB is invisible, and reproducing the bootloader's own padding
            // rules here would be a second copy of them to keep in step for no gain.
            if (off) *used = off < part->size ? off : part->size;
        }
    }
    return true;
}

// Point the bootloader at MoonBase and report whether it took. Returns false when the table has
// no factory partition, which tells the caller this device updates in place.
// NB esp_ota_set_boot_partition on a factory partition ERASES otadata rather than writing a
// sequence number: that is what makes a power cut mid-update land back in MoonBase rather than in
// a half-written app.
bool otaBootMoonBase() {
    const esp_partition_t* part = moonBasePartition();
    if (!part) return false;
    return esp_ota_set_boot_partition(part) == ESP_OK;
}

// Is the device currently executing FROM MoonBase?
bool otaRunningMoonBase() {
    const esp_partition_t* run = esp_ota_get_running_partition();
    return run && run->subtype == ESP_PARTITION_SUBTYPE_APP_FACTORY;
}

void moonbaseClearStagedUrl() {
    nvs_handle_t h;
    if (nvs_open("moonbase", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_key(h, "url");
    nvs_commit(h);
    nvs_close(h);
}

// Stage the install URL in NVS for MoonBase to consume on its next boot (see platform.h).
bool moonbaseStageInstallUrl(const char* url) {
    if (!url || !url[0]) return false;
    nvs_handle_t h;
    if (nvs_open("moonbase", NVS_READWRITE, &h) != ESP_OK) return false;
    const bool ok = nvs_set_str(h, "url", url) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

} // namespace mm::platform
