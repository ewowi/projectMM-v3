#pragma once

#include "core/MoonModule.h"
#include "core/build_info.h"   // kVersion / kRelease / kBuildDate / kFirmwareName
#include "platform/platform.h" // firmwareSize / firmwarePartition

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace mm {

// File-scope globals shared with the OTA route + the platform-layer task.
// Declared `inline` (C++17) so multiple translation units that include the
// header still share one storage instance (the header is included from
// HttpServerModule.cpp via the route, and from the module instantiation
// site in main.cpp — both must see the same g_otaStatus). An anonymous
// namespace would do the opposite — per-TU storage — which is why we
// use `inline` here.
//
// g_otaBytesRead / g_otaBytesTotal are the live byte counters the task writes.
// The UI renders them as "X KB / Y KB" via the existing progress control. The
// total starts at 0 (unknown) and flips to the real image size as soon as
// esp_https_ota_get_image_size returns it; the module's tick1s() re-binds
// the progress control when that transition happens so the static total
// captured by addProgress reflects reality (addProgress takes total by value,
// not pointer — re-bind is the cheaper alternative to widening that contract).
inline char     g_otaStatus[64]     = "idle";
inline uint32_t g_otaBytesRead      = 0;
inline uint32_t g_otaBytesTotal     = 0;

// True while an OTA is running (as opposed to idle / a terminal "success"/"failed:…").
// The URL and upload flash paths both gate their 409 "already in progress" guard on this,
// so the set of in-flight states lives in one place instead of a duplicated strcmp chain.
inline bool otaInFlight() {
    return std::strcmp(g_otaStatus, "starting")    == 0 ||
           std::strcmp(g_otaStatus, "downloading") == 0 ||
           // A PREFIX: this one carries its byte counts ("flashing: N of M bytes") so the UI can
           // draw a bar from the status alone, the same shape the MoonBase write reports in.
           std::strncmp(g_otaStatus, "flashing", 8) == 0 ||
           std::strcmp(g_otaStatus, "rebooting")   == 0 ||
           // A MoonBase install passes through its own states, and they matter MORE than the
           // app's: this window is the one where the device has no recovery image, so a second
           // install starting into a half-written factory partition is the worst case there is.
           std::strcmp(g_otaStatus, "checking")    == 0 ||
           std::strcmp(g_otaStatus, "erasing")     == 0 ||
           // A PREFIX, because this one carries its byte counts ("writing MoonBase: N of M
           // bytes") so the UI can draw a bar from the status alone. An exact compare here
           // silently stopped matching the moment those numbers were added, which would have
           // let a second install start into a half-written factory partition.
           std::strncmp(g_otaStatus, "writing MoonBase", 16) == 0;
}

/// A thin status surface for OTA flashing — surfaces flash progress as live
/// read-only controls plus the per-module status banner
///
/// Not user-configurable: `ensureInfraModules()` recreates it on every boot if
/// absent (same safety net as NetworkModule). The actual flash is driven by
/// `POST /api/firmware/url` in HttpServerModule, which hands the URL to
/// `platform::http_fetch_to_ota()` — a task that downloads via `esp_https_ota` and
/// writes the next OTA partition, communicating through the file-scope globals above
/// (`g_otaStatus`, `g_otaBytesRead`, `g_otaBytesTotal`). This module polls them in
/// `tick1s()` and copies into its bound control buffers so the WebSocket state push
/// picks up the change at 1 Hz. The shared-buffer + 1 Hz poll pattern is the simplest
/// way to bridge a FreeRTOS task and a MoonModule on the scheduler thread without
/// locks; no synchronisation, since torn reads of display-only fields are acceptable.
///
/// **Controls.** `version` — pure semver (`MM_VERSION`): a stable release is a clean
/// `X.Y.Z`, a moving `latest` build is a monotonic prerelease `` `<core>-dev.<N>` `` (semver.org
/// §9/§11), so the release channel is derivable from the version rather than mixed into
/// it, keeping the string a clean machine-comparable semver the UI's update check compares
/// against the newest GitHub release. `build` — build date/time. `firmware` — the
/// build-time variant key (`esp32`, `esp32-eth`, `esp32s3-n16r8`, … / `desktop-*`) that
/// identifies which release asset matches the device (a legacy `esp32-eth-wifi` key
/// OTA-maps to `esp32`); the physical hardware is SystemModule's `deviceModel`.
/// `partition`: the selected image's size against the slot that holds it. During an install it
/// shows the INCOMING image filling that same slot, which is the same quantity measured live.
/// `image` selects WHICH of a device's two images the four controls above describe: the running
/// app, or MoonBase in the factory slot (read from its app descriptor). Present only where there
/// are two, and hidden, because the card draws it as a tab strip rather than as a setting.
///
/// **Progress is not a control.** The byte counters below drive the overlay the UI raises while
/// an install runs; a card row would sit at zero for the whole life of a device that is not
/// mid-install.
///
/// **Flash phase is not a control** — it surfaces through the module's shared status slot
/// (`setStatus()`): `idle` clears the banner, an `error:` prefix maps to `Severity::Error`,
/// everything else (`starting`/`downloading`/`flashing`/`rebooting`) is neutral. On
/// desktop (`platform::hasOta == false`) the controls still exist for UI uniformity but the
/// route returns 501 and status stays "idle" forever.
///
/// **Flash lifecycle.** A `POST /api/firmware/url` (HttpServerModule, body `{"url": …}`) writes
/// `starting` and spawns the platform OTA task, which walks: `downloading` → `esp_https_ota_begin`
/// opens the TLS connection and follows redirects (a GitHub release URL 302s to
/// `release-assets.githubusercontent.com`) → `flashing`, `esp_https_ota_perform` advancing
/// the byte counters → `esp_https_ota_finish` commits the image to the next OTA partition and flips
/// the boot pointer → `rebooting`, a ~600 ms delay so the HTTP response reaches the browser first,
/// then `esp_restart()`. The device boots the new image and the UI re-reads `version` / `firmware`.
///
/// **Error taxonomy** (all via the status slot, `error: ` prefix, staying until the next POST):
/// `error: ota begin <IDF name>` (DNS / TLS / no OTA partition), `error: ota perform <IDF name>`
/// (network drop mid-download), `error: incomplete download` (size mismatch), `error: ota finish
/// <IDF name>` (commit / boot-flip), `error: task create failed` (`xTaskCreate` OOM — no retry). A
/// wrong-firmware binary fails at `esp_https_ota_begin` (chip-family mismatch) or boot (partition
/// mismatch) — recoverable by a USB re-flash, not a brick.
///
/// **Compatibility** is the *caller's* responsibility (the install-picker's `isCompatible()`): strip
/// `-eth*` from both firmware keys, equal identities are compatible — so `esp32` and `esp32-eth` are
/// mutually OTA-compatible (same chip, different feature flags), the legacy `esp32-eth-wifi` key
/// strips to `esp32`, and `esp32s3-n16r8` is only itself.
///
/// **MoonBase devices** (4 MB tables; the read-only `moonbase` control marks one): the app cannot
/// flash itself: one app slot, and it is running from it. `POST /api/firmware/url` stages the URL
/// in NVS and reboots into the MoonBase factory image, which installs unattended and reboots back;
/// uploads re-POST from the browser once MoonBase answers. See architecture.md, MoonBase:
/// the second boot image.
///
/// **Prior art:** `esp_https_ota` is the standard ESP-IDF OTA-from-HTTP component used by every ESP32
/// OTA flow since IDF v4.x; the install-picker UI is the new layer on top. The MoonBase scheme,
/// a minimal boot image in place of a second OTA slot: follows Tasmota's safeboot and Mathieu
/// Carbou's MycilaSafeBoot (https://github.com/mathieucarbou/MycilaSafeBoot), rewritten minimal
/// against ESP-IDF (moonbase/).
/// @card FirmwareUpdateModule.png
class FirmwareUpdateModule : public MoonModule {
public:
    /// Diagnostics keep ticking regardless of the user toggle; matches
    /// SystemModule + NetworkModule. The user can't easily re-enable a
    /// disabled diagnostic module without it being visible.
    bool respectsEnabled() const MM_NONBLOCKING override { return false; }

    void setup() override {
        // Copy the file-scope globals into the bound buffers on boot so the
        // first WS state push surfaces a coherent "idle" / 0 pair.
        std::snprintf(statusStr_, sizeof(statusStr_), "%s", g_otaStatus);   // always NUL-terminates
        publishStatus();
        totalSnap_ = g_otaBytesTotal;

        // Firmware identity (static for this build). version is PURE SEMVER (kVersion from
        // library.json): a clean "2.0.0" on a stable release, or a prerelease like "2.1.0-dev" on a
        // moving/dev build (semver.org §9 — the prerelease suffix is how a not-yet-released build is
        // expressed). The release channel is derivable from the version itself (a prerelease suffix
        // means "not a stable release"), so it is NOT mixed into the string; kRelease stays the
        // separate build-channel tag (which git tag this binary shipped under) without polluting the
        // machine-comparable version. This keeps `version` a clean semver the UI's update check can
        // compare against the newest GitHub release.
        std::snprintf(versionStr_, sizeof(versionStr_), "%s", kVersion);
        // `build` carries the git BUILD ID first, then the compile timestamp: "0d75fdbe+ · Jul 16 2026
        // 14:51:02". The id is what actually answers "which code is on this board?" — kBuildDate alone
        // cannot, because __DATE__ expands when the TU including build_info.h compiles, so it freezes
        // while the firmware moves on (a stale date reads as a failed flash; see build_info.h).
        std::snprintf(buildStr_, sizeof(buildStr_), "%s · %s", kBuildId, kBuildDate);
        std::snprintf(firmwareStr_, sizeof(firmwareStr_), "%s", kFirmwareName);
        readMoonBaseVersion();
    }

    /// WHICH MOONBASE this device carries, read from the factory partition rather than assumed.
    /// MoonBase is the one image nothing else updates, so it drifts: a bench board that could not
    /// install firmware was running a MoonBase built long before its app, and nothing on the
    /// device said so. Both strings come from the same computed version, so a mismatch means the
    /// two images were built apart, which is exactly what to surface.
    ///
    /// Re-read after installing a new one, not only at setup: rebuildControls() re-runs
    /// defineControls(), which re-binds this buffer but never refills it, so a card refreshed
    /// without this kept showing the version of the image that was just replaced.
    void readMoonBaseVersion() {
        char installed[32] = {};
        if (platform::otaMoonBaseVersion(installed, sizeof(installed))) {
            const bool matches = std::strcmp(installed, kVersion) == 0;
            std::snprintf(moonbaseStr_, sizeof(moonbaseStr_), "%s%s",
                          installed, matches ? "" : " (outdated)");
        }
        // The rest of its identity, so the UI can describe MoonBase with the same four fields it
        // describes the app with: one row builder serves whichever image the user selected.
        platform::otaMoonBaseBuild(moonbaseBuildStr_, sizeof(moonbaseBuildStr_));
    }

    void defineControls() override {
        // Firmware identity for the SELECTED image, queried here (idempotent, no I/O) so the
        // gate sees a real total.
        // ONE SET OF CONTROLS, describing whichever image `image` selects. A device carries two
        // (the app it runs, and MoonBase the recovery image) and both are described by the same
        // four facts, so publishing eight controls would be the same fact twice under two names,
        // and a UI drawing its own copies underneath would be a third. The selector re-runs this
        // (onControlChanged below), the way MoonLiveService rebuilds when its script changes.
        if (platform::otaHasMoonBase()) {
            static const char* const kImages[] = { "App", "MoonBase" };
            controls_.addSelect("image", imageSel_, kImages, 2);
            // Drawn by the card as a TAB STRIP, not by the generic control list: it selects which
            // image every row below describes, which reads as a tab rather than as one setting
            // among the settings it governs. Same convention as the File Manager's own panel
            // controls: hidden here so the value never renders twice.
            controls_.setHidden(controls_.count() - 1, true);
        } else {
            imageSel_ = 0;   // nothing else to describe: the app is the only image
        }

        if (imageSel_ == 0) {
            controls_.addReadOnly("version", versionStr_, sizeof(versionStr_));
            controls_.addReadOnly("build", buildStr_, sizeof(buildStr_));
            controls_.addReadOnly("firmware", firmwareStr_, sizeof(firmwareStr_));
            firmwareSizeVal_ = static_cast<uint32_t>(platform::firmwareSize());
            totalFlashVal_ = static_cast<uint32_t>(platform::firmwarePartition());
            if (totalFlashVal_ > 0) {
                controls_.addProgress("partition", firmwareSizeVal_, totalFlashVal_);
            }
        } else {
            // Read here rather than trusting a value setup() left behind: this runs on every
            // rebuild, and a member filled once at setup is one refactor away from being read
            // before it is written. That is exactly what went wrong on the bench.
            readMoonBaseVersion();
            platform::otaMoonBaseSize(&moonbaseSizeVal_, &moonbaseTotalVal_);
            controls_.addReadOnly("version", moonbaseStr_, sizeof(moonbaseStr_));
            controls_.addReadOnly("build", moonbaseBuildStr_, sizeof(moonbaseBuildStr_));
            // MoonBase has no variant: one image per chip, since it carries no board-specific
            // code. The chip is what names its release asset, so that is what this reports.
            std::snprintf(moonbaseChipStr_, sizeof(moonbaseChipStr_), "%s", platform::chipModel());
            controls_.addReadOnly("firmware", moonbaseChipStr_, sizeof(moonbaseChipStr_));
            if (moonbaseTotalVal_ > 0) {
                controls_.addProgress("partition", moonbaseSizeVal_, moonbaseTotalVal_);
            }
        }

        // OTA status goes through MoonModule::setStatus() (the per-module status
        // slot every module shares), not a bespoke read-only control — same
        // choice DevicesModule made. The error-prefixed states map to
        // Severity::Error; everything else is neutral. See publishStatus().
        //
        // There is no progress CONTROL: an install's progress belongs in the overlay the UI puts
        // up while it runs, not in a card row that sits at zero for the life of the device. The
        // counters below still drive it, read from the status the same overlay polls.
    }

    /// Switching `image` changes which image every other control describes, so the set is rebuilt.
    /// Same mechanism MoonLiveService uses when a script redeclares its controls.
    void onControlChanged(const char* controlName) override {
        if (std::strcmp(controlName, "image") == 0) rebuildControls();
    }

    void tick1s() MM_NONBLOCKING override {
        // Poll the OTA task's progress + status. No locks: the writer is
        // a single task, reads are atomic at this granularity, and a torn
        // read shows as a brief mid-update glimpse — visually harmless.
        std::snprintf(statusStr_, sizeof(statusStr_), "%s", g_otaStatus);   // always NUL-terminates
        publishStatus();
        // A MoonBase install finishes on its own task with no reboot to announce it, so the
        // version this card shows goes stale the moment the write completes. Watching the status
        // fall back out of the installing states is what notices.
        const bool installing = std::strcmp(statusStr_, "checking") == 0 ||
                                std::strcmp(statusStr_, "erasing") == 0 ||
                                std::strncmp(statusStr_, "writing MoonBase", 16) == 0;
        if (wasInstallingMoonBase_ && !installing) {
            readMoonBaseVersion();
            rebuildControls();
        }
        wasInstallingMoonBase_ = installing;

        // THE PARTITION BAR DOUBLES AS THE INSTALL BAR. At rest it shows how much of the slot the
        // installed image fills, which is what a user wants to know about a partition. During an
        // install it shows the INCOMING image filling that same slot, which is the same quantity
        // measured live, so the card itself shows activity rather than only a popup: a URL
        // install on a device with no MoonBase had no bar at all, just a line of status text.
        //
        // The control binds this member by reference, so pointing it at the live count is enough;
        // no second control, and no re-bind. Restored from the real image size when the install
        // ends, which also corrects it after a MoonBase install that does not reboot.
        const bool writing = otaInFlight();
        if (writing) {
            firmwareSizeVal_ = g_otaBytesRead;
            moonbaseSizeVal_ = g_otaBytesRead;
        } else if (wasWriting_) {
            firmwareSizeVal_ = static_cast<uint32_t>(platform::firmwareSize());
            platform::otaMoonBaseSize(&moonbaseSizeVal_, &moonbaseTotalVal_);
        }
        wasWriting_ = writing;

        // Re-bind on total transition. Only fires once per OTA (and once on
        // any later OTA the user starts — we deliberately don't reset the
        // total to 0 between updates; the previous value is a fine starting
        // estimate until the new task reports the new size). rebuildControls
        // re-runs defineControls() so the addProgress' captured `aux` (total)
        // is refreshed to the new totalSnap_ value.
        if (g_otaBytesTotal != totalSnap_) {
            totalSnap_ = g_otaBytesTotal;
            rebuildControls();   // re-bind the progress total the overlay reads
        }
    }

    /// Point the shared status slot at our owned buffer, choosing the severity
    /// from the status text: the platform OTA task prefixes every failure with
    /// "error: " (see platform_esp32_ota.cpp), so that prefix is the Error gate.
    /// "idle" is the quiescent state and reads better as no banner than as an
    /// info banner, so it clears the slot. setStatus doesn't copy — statusStr_
    /// outlives every call, so the pointer stays valid.
    void publishStatus() {
        if (std::strcmp(statusStr_, "idle") == 0) {
            clearStatus();
        } else {
            setStatus(statusStr_,
                      std::strncmp(statusStr_, "error:", 6) == 0 ? Severity::Error
                                                                 : Severity::Status);
        }
    }

private:
    char     statusStr_[64] = "idle";
    uint32_t totalSnap_     = 0;
    // Firmware identity (static for this build) + the running app-partition usage.
    char     versionStr_[32] = {};   ///< pure semver — such as "2.0.0" or "2.1.0-dev.7"
    char     buildStr_[48]   = {};   // "<8-hex><+?> · <__DATE__ __TIME__>" — id + separator + 20-char stamp
    char     firmwareStr_[24] = {};  ///< build variant name, such as "esp32s3-n16r8"
    uint32_t firmwareSizeVal_ = 0;   ///< bytes used in the app partition
    uint32_t totalFlashVal_   = 0;   ///< app partition size
    /// The installed MoonBase's version, plus an "(outdated)" mark when it differs from this
    /// app's. Falls back to "standby" when the image carries no readable descriptor, which is
    /// every MoonBase built before it reported a version.
    char     moonbaseStr_[48] = "standby";
    char     moonbaseBuildStr_[32] = {};   ///< when the installed MoonBase was built
    uint32_t moonbaseSizeVal_  = 0;        ///< bytes its image occupies
    uint32_t moonbaseTotalVal_ = 0;        ///< the factory slot's size
    char     moonbaseChipStr_[16] = {};    ///< the chip whose MoonBase image this board takes
    uint8_t  imageSel_ = 0;                ///< 0 = the app, 1 = MoonBase: which image is described
    bool     wasWriting_ = false;          ///< edge-detects the end of any install, to restore the bar
    bool     wasInstallingMoonBase_ = false;   ///< edge-detects the end of a MoonBase install
};

} // namespace mm
