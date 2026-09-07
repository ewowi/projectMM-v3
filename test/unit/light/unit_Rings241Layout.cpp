// @module Rings241Layout
// @also SingleColumnLayout

// Pins the 241-LED disc's wiring order. The disc is soldered either from the center LED outward
// (MoonLight's order, the default) or from the outer 60-LED ring inward, and a layout that only
// knew one would light the wrong ring for the other. The coordinates are the same set either way;
// what `outside in` changes is which index lands on which ring.

#include "doctest.h"
#include "light/layouts/Rings241Layout.h"

#include <algorithm>
#include <vector>

namespace {

struct CoordEntry {
    mm::nrOfLightsType idx;
    mm::lengthType x, y, z;
    bool operator==(const CoordEntry& o) const { return x == o.x && y == o.y && z == o.z; }
};

void collectCoord(void* ctx, mm::nrOfLightsType idx, mm::lengthType x, mm::lengthType y, mm::lengthType z) {
    static_cast<std::vector<CoordEntry>*>(ctx)->push_back({idx, x, y, z});
}

std::vector<CoordEntry> coordsOf(const mm::LayoutBase& layout) {
    std::vector<CoordEntry> out;
    layout.placeLights(mm::CoordSink{collectCoord, nullptr, &out});
    return out;
}

constexpr mm::nrOfLightsType kTotal = 241;
constexpr mm::nrOfLightsType kOuter = 60;   // the largest ring, wired first when outside in

} // namespace

// Indices are contiguous 0..240 whatever the wiring order: a gap or a repeat is a light the driver
// never writes, so the disc would carry a permanently dark pixel.
TEST_CASE("Rings241 emits 241 consecutively indexed lights in either wiring order") {
    for (bool outsideIn : {false, true}) {
        mm::Rings241Layout disc;
        disc.outside_in = outsideIn;
        CHECK(disc.lightCount() == kTotal);
        const auto coords = coordsOf(disc);
        REQUIRE(coords.size() == kTotal);
        for (mm::nrOfLightsType i = 0; i < kTotal; i++) CHECK(coords[i].idx == i);
    }
}

// The default wires from the center out: light 0 IS the center LED (the 1-LED ring), and the last
// sixty lights are the outer ring. This is MoonLight's order, so an existing disc keeps its picture.
TEST_CASE("Rings241 wires from the center outward by default") {
    mm::Rings241Layout disc;
    const auto coords = coordsOf(disc);
    REQUIRE(coords.size() == kTotal);
    // The center is the shared origin every ring is drawn around; the 1-LED ring sits exactly on it.
    const auto& center = coords[0];
    // Every outer-ring light is farther from the center than every inner light: the outer sixty
    // must be the last sixty, or the wiring order is not center-out.
    auto dist2 = [&](const CoordEntry& c) {
        const int dx = c.x - center.x, dy = c.y - center.y;
        return dx * dx + dy * dy;
    };
    int innerMax = 0;
    for (mm::nrOfLightsType i = 1; i < kTotal - kOuter; i++) innerMax = std::max(innerMax, dist2(coords[i]));
    for (mm::nrOfLightsType i = kTotal - kOuter; i < kTotal; i++) CHECK(dist2(coords[i]) > innerMax);
}

// Outside in flips the ring SEQUENCE and nothing else: the first sixty lights are the outer ring in
// the same direction they were wired center-out, and the center LED is last. Asserted against the
// default's output ring by ring, so a reversal that also flipped the direction around a ring, or
// that moved a coordinate, is caught rather than passing on a symmetric disc.
TEST_CASE("outside in puts light 0 on the outer ring and the center LED last, rings unchanged inside") {
    mm::Rings241Layout in, out;
    out.outside_in = true;
    const auto a = coordsOf(in);
    const auto b = coordsOf(out);
    REQUIRE(a.size() == kTotal);
    REQUIRE(b.size() == kTotal);

    // Walk the default's rings from the last (outer) to the first (center) and expect them to appear
    // in `out` in that order, each ring's own LED order intact.
    constexpr mm::nrOfLightsType sizes[9] = {1, 8, 12, 16, 24, 32, 40, 48, 60};
    mm::nrOfLightsType starts[9];
    mm::nrOfLightsType acc = 0;
    for (int r = 0; r < 9; r++) { starts[r] = acc; acc += sizes[r]; }

    mm::nrOfLightsType o = 0;
    for (int r = 8; r >= 0; r--) {
        for (mm::nrOfLightsType i = 0; i < sizes[r]; i++) {
            CHECK(b[o] == a[starts[r] + i]);   // same coordinate, same position within the ring
            CHECK(b[o].idx == o);
            o++;
        }
    }
    CHECK(o == kTotal);
    CHECK(b[kTotal - 1] == a[0]);              // the center LED is the last light when wired outside in
}

// `angleFirst` says where light 0 of each ring sits, in degrees from the bottom. A disc is soldered
// with its first LED wherever the builder started, so without it the image is fixed against the
// hardware. The default of 0 must reproduce MoonLight's placement exactly, since that is what every
// other test here pins.
TEST_CASE("angleFirst moves the first light around the ring, and 0 leaves the disc where it was") {
    mm::Rings241Layout plain;
    const auto at0 = coordsOf(plain);

    mm::Rings241Layout alsoZero;
    alsoZero.angleFirst = 0;
    CHECK(coordsOf(alsoZero) == at0);          // the default is not merely near MoonLight's, it IS it

    // A quarter turn moves every ring's first light. The disc is symmetric, so the SET of
    // coordinates barely changes; what moves is which index sits where, which is the point.
    mm::Rings241Layout turned;
    turned.angleFirst = 90;
    const auto at90 = coordsOf(turned);
    REQUIRE(at90.size() == at0.size());
    CHECK_FALSE(at90 == at0);

    // The center LED has no angle, so it cannot move however far the disc is turned.
    CHECK(at90[0] == at0[0]);

    // 359 is one degree short of a full turn, so the disc is nearly back where it started: the
    // control stops at 359 because 360 IS 0 (the ring closes) and offering both invites the
    // question of which one it is.
    mm::Rings241Layout almost;
    almost.angleFirst = 359;
    const auto at359 = coordsOf(almost);
    for (size_t i = 0; i < at0.size(); i++) {
        CHECK(std::abs(int(at359[i].x) - int(at0[i].x)) <= 1);
        CHECK(std::abs(int(at359[i].y) - int(at0[i].y)) <= 1);
    }
}
