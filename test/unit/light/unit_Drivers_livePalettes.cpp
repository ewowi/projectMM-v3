// @module Drivers
// @also Palette, MoonLivePalette

// How a DOWNLOADED palette becomes a selectable one.
//
// A scripted palette arrives as a `.mlp` file the UI writes to the device, and three separate things
// have to agree before a user can pick it: the scan has to FIND the file, the picker has to LIST it,
// and the `palette` control has to ACCEPT its index. Each of those failed independently in the
// field, and each failure looked the same from the outside ("I downloaded it and nothing happened"),
// so they are pinned separately here.

#include "doctest.h"
#include "light/drivers/Drivers.h"
#include "light/Palette.h"
#include "light/moonlive/MoonLiveScriptFile.h"
#include "platform/platform.h"
#include "../core/conditional_controls.h"   // mm::test::controlIndex

#include <cstdio>
#include <cstring>
#include <filesystem>

using namespace mm;

namespace {

/// The smallest thing that stands in for a palette script. These tests never RUN it: they are about
/// DISCOVERY (which files are found and offered), and what a scripted palette paints is covered by
/// MoonLivePalette's own tests.
constexpr const char* kPaletteSrc = "void setup() {}\n";

/// An empty filesystem root of its own per test, so a count assertion means what it says: the
/// developer's own device directory carries whatever palettes they have been trying, and a test
/// that counted those would pass or fail by accident. fsSetRoot is provided for exactly this
/// (platform.h), and the FilesystemModule persistence tests use the same shape.
struct IsolatedFs {
    char root[256];
    explicit IsolatedFs(const char* tag) {
        std::snprintf(root, sizeof(root), "/tmp/mm_palettes_%s_%u", tag,
                      static_cast<unsigned>(platform::millis()));
        std::filesystem::remove_all(root);
        platform::fsSetRoot(root);
        REQUIRE(platform::fsMount());
    }
    ~IsolatedFs() {
        platform::fsSetRoot("");        // back to the real root for every other test
        std::filesystem::remove_all(root);
    }
};

/// Write one `.mlp` into a script directory, creating the directory first: fsWriteAtomic does not
/// create parents, exactly as POST /api/file does not.
void writePalette(const char* dir, const char* name) {
    platform::fsMkdir(dir);
    char path[160];
    std::snprintf(path, sizeof(path), "%s/%s", dir, name);
    REQUIRE(platform::fsWriteAtomic(path, kPaletteSrc, std::strlen(kPaletteSrc)));
}

/// The highest index the `palette` control ACCEPTS. This is the number every one of these bugs
/// moved: the control carries its ceiling in `max`, and a value above it is refused with "value out
/// of range", which is what made a freshly downloaded palette unselectable even once it was listed.
int32_t paletteMax(Drivers& drv) {
    const int i = test::controlIndex(drv, "palette");
    REQUIRE(i >= 0);
    return drv.controls()[static_cast<uint8_t>(i)].max;
}

}  // namespace

TEST_CASE("a palette downloaded to the factory directory is offered by the picker") {
    // THE FIELD BUG: the UI downloads a `.mlp` to the FACTORY directory (`/.moonlive`) so that a
    // later edit can shadow it, but the scan only ever read the USER directory (`/moonlive`). The
    // file was on the device and could never be listed, so the picker said "reopen the picker to
    // select it" and reopening changed nothing, forever.
    IsolatedFs fs("factory");
    Drivers drv;
    drv.defineControls();
    const int32_t before = paletteMax(drv);

    writePalette(mm::moonlive::kFactoryScriptDir, "unit-factory.mlp");
    drv.rebuildControls();      // CLEARS then re-defines, which is what a schema change does

    CHECK(paletteMax(drv) == before + 1);
}

TEST_CASE("editing a factory palette leaves one entry, not two") {
    // Editing a factory script SAVES A SECOND FILE of the same name to the user directory, which
    // then shadows the factory copy (resolveScript prefers it). Both directories are scanned, so
    // without a dedupe the same palette would appear twice in the picker, and one of those two rows
    // would load a file the user cannot see.
    IsolatedFs fs("both");
    Drivers drv;
    drv.defineControls();
    const int32_t before = paletteMax(drv);

    writePalette(mm::moonlive::kFactoryScriptDir, "unit-both.mlp");
    writePalette(mm::moonlive::kScriptDir, "unit-both.mlp");
    drv.rebuildControls();      // CLEARS then re-defines, which is what a schema change does

    CHECK(paletteMax(drv) == before + 1);   // ONE entry, from two files
}

TEST_CASE("a palette added while running becomes selectable without a reboot") {
    // THE BLOCKER BEHIND THE OTHER TWO: `palette`'s ceiling is baked when the control is defined
    // (the scripted count plus the built-ins), while a downloaded file is discovered by prepare().
    // So the new palette was LISTED and then REFUSED with "value out of range": the picker offered
    // a row that could not be chosen, and only a reboot fixed it. prepare() therefore rebuilds the
    // controls when the count moves.
    IsolatedFs fs("late");
    Drivers drv;
    drv.defineControls();
    const int32_t before = paletteMax(drv);

    // Arriving AFTER the control was defined is the whole point: this is a download onto a running
    // device, not a file that was present at boot.
    writePalette(mm::moonlive::kFactoryScriptDir, "unit-late.mlp");
    drv.prepare();

    CHECK(paletteMax(drv) == before + 1);
}
