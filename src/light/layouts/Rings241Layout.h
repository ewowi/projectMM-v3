#pragma once

#include "light/layouts/LayoutBase.h"
#include <numbers>

namespace mm {

// The classic 241-LED concentric-ring disc: nine full circles sharing one
// center, with ring LED counts 1, 8, 12, 16, 24, 32, 40, 48, 60 (sum 241).
//
// Prior art: MoonLight's Rings241Layout, which composes MoonLight's RingLayout
// once per ring. RingLayout places `n` LEDs evenly on a circle of radius
// n / (2π), starting at the bottom (angleRad = π at i=0) and stepping by 2π/n.
// This port reproduces that exact per-LED math but emits coordinates only;
// MoonLight's pin/wiring plumbing (doNextPin/nextPin, and RingLayout's
// angleFirst/rotation/clockwise/nrOfLEDs UI controls) has no place here, since
// a projectMM layout hands positions to the driver and the driver owns pins.
// The one geometry control that survives is `scale`, RingLayout's spacing
// multiplier. Every ring is a full circle (MoonLight's rotation = 360), so every
// LED is emitted; that makes lightCount() the fixed constant 241.
//
// `outside in` is ours, not MoonLight's: it names which end of the wire is light
// 0. A disc is soldered either from the center LED outward (MoonLight's order,
// the default) or from the outer 60-LED ring inward, and a layout that only knew
// one of them would light the wrong ring for the other. Only the ring SEQUENCE
// flips; the direction around each ring stays as wired.
//
// Precision is reproduced statement-for-statement, because the disc's integer
// coordinates depend on it: MoonLight forms `angleRad` from the double macros
// PI / TWO_PI, stores it in a *float*, then takes float sinf/cosf and multiplies
// a float radius. Doing the angle in all-float (a float π constant) or keeping
// it all-double both shift ring points that land exactly on an integer axis
// (cos/sin = 0) to the wrong side of the (int) truncation, differing from
// MoonLight by one unit. The faithful path is MoonLight's own: form the angle in
// double → narrow to float → float trig → float radius.
// Author: MoonLight — https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Nodes/Layouts/L_MoonLight.h
/// Layout of the 241-LED concentric-rings disc.
class Rings241Layout : public LayoutBase {
public:
    const char* tags() const override { return "💫"; }
    Dim dimensions() const override { return Dim::D2; }
    // Spacing multiplier: scales both the ring radii and the shared center.
    // MoonLight default 2, range 1..10.
    uint8_t scale = 2;
    // Wiring order: false = light 0 at the center, rings outward (MoonLight's
    // order); true = light 0 on the outer ring, rings inward.
    bool outside_in = false;   // "outside in"
    // Where light 0 of each ring sits, in degrees from the bottom. Named for
    // RingLayout's control of the same meaning, NOT `rotation`: that word is
    // taken in this family for the arc a partial ring spans, and one word with
    // two meanings across two ring layouts is worse than a longer name. A disc
    // is soldered with its first LED wherever the builder started, so without
    // this the image is fixed against the hardware. 0 keeps MoonLight's
    // placement, which is why it is the default.
    uint16_t angleFirst = 0;

    void defineControls() override {
        controls_.addControl("scale", scale, 1, 10);
        controls_.addControl("outside in", outside_in);
        controls_.addControl("angleFirst", angleFirst, 0, 359);
    }

    nrOfLightsType lightCount() const override {
        // Fixed by construction: the nine ring sizes always sum to 241, and every
        // ring is a full circle so no LED is culled. Kept in lockstep with
        // placeLights below (same kRingSizes sum).
        nrOfLightsType total = 0;
        for (uint8_t n : kRingSizes) total += n;
        return total;  // 1+8+12+16+24+32+40+48+60 = 241
    }

    void placeLights(const CoordSink& sink) const override {
        // Shared center. MoonLight: leftMargin = 1.1 * getRadius(60), assigned to
        // a uint8_t (implicit truncation), stored as ringCenter's integer x/y, then
        // scaled per LED: x = scale * ringCenter.x.
        const uint8_t leftMargin = static_cast<uint8_t>(1.1f * getRadius(60));

        nrOfLightsType idx = 0;
        // Rings emitted smallest-to-largest, the same order MoonLight calls
        // RingLayout::onLayout (nrOfLEDs = 1, 8, 12, … 60), or largest-to-smallest
        // when the disc is wired from the outer ring in. The per-LED math below
        // depends only on n, so the ring order is the whole difference.
        constexpr uint8_t kRings = sizeof(kRingSizes) / sizeof(kRingSizes[0]);
        for (uint8_t r = 0; r < kRings; r++) {
            const uint8_t n = kRingSizes[outside_in ? kRings - 1 - r : r];
            const float radius = getRadius(n);
            for (uint8_t i = 0; i < n; i++) {
                float x = static_cast<float>(scale * leftMargin);
                float y = static_cast<float>(scale * leftMargin);
                if (n != 1) {
                    // RingLayout's angleRad = π + 2π·i / n + 2π·angleFirst / 360.
                    // Formed in double (MoonLight's PI/TWO_PI are double macros),
                    // then narrowed to float for the float sinf/cosf below. The
                    // angleFirst term rides in the same double expression so an
                    // angleFirst of 0 is bit-identical to MoonLight's placement.
                    const float angleRad = static_cast<float>(
                        kPi + (kTwoPi * static_cast<double>(i)) / static_cast<double>(n)
                            + (kTwoPi * static_cast<double>(angleFirst)) / 360.0);
                    x -= scale * std::sin(angleRad) * radius;
                    y += scale * std::cos(angleRad) * radius;
                }
                // MoonLight truncates each axis with a (int) cast (toward zero);
                // z is scale * ringCenter.z = scale * 0 = 0. Preserve exactly.
                sink.pixel(idx++,
                   static_cast<lengthType>(static_cast<int>(x)),
                   static_cast<lengthType>(static_cast<int>(y)),
                   0);
            }
        }
    }

private:
    // RingLayout::getRadius(n) = n / (2π), the radius that spaces n LEDs one
    // unit apart around the circle. Returns float, as MoonLight does (the
    // division is in double then narrowed, matching n / TWO_PI on the double macro).
    static float getRadius(uint8_t n) { return static_cast<float>(n / kTwoPi); }

    static constexpr double kPi = std::numbers::pi;
    static constexpr double kTwoPi = 2.0 * kPi;

    // The nine ring sizes of the 241-LED disc, inner to outer.
    static constexpr uint8_t kRingSizes[9] = {1, 8, 12, 16, 24, 32, 40, 48, 60};
};

} // namespace mm