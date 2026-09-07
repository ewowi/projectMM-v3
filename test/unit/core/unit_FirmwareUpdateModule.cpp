// @module FirmwareUpdateModule

#include "doctest.h"
#include "core/FirmwareUpdateModule.h"
#include <cstring>

// The `firmware` control is always present and non-empty (either a real firmware key from
// build_info.h or the fallback "unknown"). The firmware card owns firmware identity
// (version/build/firmware) + the partition usage.
TEST_CASE("FirmwareUpdateModule firmware control populated") {
    // The firmware control is wired in setup() from kFirmwareName (build_info.h).
    // Local desktop builds fall through to "unknown" because CMake doesn't
    // pass -DMM_FIRMWARE_NAME; release builds get the real key. Either way,
    // the control must exist and be non-empty so the OTA / install-picker path
    // has something to read. (See docs/architecture.md § Firmware vs board —
    // "firmware" is the compiled-binary variant; the physical board is separate.)
    mm::FirmwareUpdateModule fw;
    fw.setup();
    fw.defineControls();

    bool found = false;
    for (uint8_t i = 0; i < fw.controls().count(); i++) {
        if (std::strcmp(fw.controls()[i].name, "firmware") == 0) {
            CHECK(fw.controls()[i].type == mm::ControlType::ReadOnly);
            const char* val = static_cast<const char*>(fw.controls()[i].ptr);
            CHECK(val != nullptr);
            CHECK(val[0] != '\0');  // non-empty (either a real key or "unknown")
            found = true;
        }
    }
    CHECK(found);

    // version + build are part of this module's firmware-identity controls, so they're present too.
    // firmwarePartition is gated on platform::firmwarePartition() > 0 (the app-partition size), so it
    // appears on a real device but NOT on desktop/test where firmwarePartition() returns 0 — assert
    // its TYPE only when present (a Progress control), rather than its presence, so this stays valid
    // on both.
    bool hasVersion = false, hasBuild = false, hasPartition = false;
    for (uint8_t i = 0; i < fw.controls().count(); i++) {
        const auto& c = fw.controls()[i];
        if (std::strcmp(c.name, "version") == 0) hasVersion = true;
        if (std::strcmp(c.name, "build") == 0) hasBuild = true;
        // `partition`, renamed from `firmwarePartition` when one control set began describing
        // either image. The old name left this CHECK inside a condition that is never true, so it
        // asserted nothing while looking like it did.
        if (std::strcmp(c.name, "partition") == 0) {
            hasPartition = true;
            CHECK(c.type == mm::ControlType::Progress);
        }
    }
    CHECK(hasVersion);
    CHECK(hasBuild);
    // Not asserted present: the control is added only when the platform reports a partition size,
    // and desktop reports 0. What is asserted is its TYPE when it does exist, which is what the
    // old `firmwarePartition` spelling silently stopped checking.
    (void)hasPartition;
}

// OTA phase is surfaced through the shared status slot (MoonModule::setStatus()),
// not a control. publishStatus() runs in setup()/tick1s() and maps the platform
// OTA status string to a severity: "idle" clears the banner, an "error: " prefix
// is Severity::Error, anything else is neutral Severity::Status.
TEST_CASE("FirmwareUpdateModule OTA status routes through the status slot") {
    mm::FirmwareUpdateModule fw;

    // Boot state: g_otaStatus is "idle" → no banner.
    std::strncpy(mm::g_otaStatus, "idle", sizeof(mm::g_otaStatus));
    fw.setup();
    CHECK(fw.status() == nullptr);

    // An in-flight phase shows as a neutral status banner.
    std::strncpy(mm::g_otaStatus, "downloading", sizeof(mm::g_otaStatus));
    fw.tick1s();
    REQUIRE(fw.status() != nullptr);
    CHECK(std::strcmp(fw.status(), "downloading") == 0);
    CHECK(fw.severity() == mm::MoonModule::Severity::Status);

    // A failure (platform prefixes every failure with "error: ") is an error banner.
    std::strncpy(mm::g_otaStatus, "error: ota perform ESP_FAIL", sizeof(mm::g_otaStatus));
    fw.tick1s();
    REQUIRE(fw.status() != nullptr);
    CHECK(fw.severity() == mm::MoonModule::Severity::Error);

    // Returning to idle clears the banner again.
    std::strncpy(mm::g_otaStatus, "idle", sizeof(mm::g_otaStatus));
    fw.tick1s();
    CHECK(fw.status() == nullptr);
}
