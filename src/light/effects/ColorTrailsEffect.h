#pragma once

#include "light/effects/EffectBase.h"

namespace mm {

// ColorTrails: emitters pouring color into a flow that is two noise profiles, not a field.
//
// The flow here is SEPARABLE, and that is the whole idea. A velocity field normally costs one
// vector per cell; this one is a single noise value per row and per column, so a WxH grid is
// steered by W+H numbers. Each row shifts horizontally by its own amount and each column shifts
// vertically by its own, and because the two shears compose, the picture swirls and folds as though
// something were solving for it. Nothing is: there is no pressure, no divergence, no iteration.
//
// That makes it the cheap end of the transport family. `FluidEffect` runs a real Stam solver and
// buys interaction between jets and vortices that form out of the flow's own history, at roughly
// twenty passes over the grid; this is two, and on a large panel that is the difference between an
// effect that runs and one that does not. Reach for the solver when the medium itself is the
// subject, and for this when the subject is color being carried.
//
// The two tiers are Jeff's own arrangement, not our contrast with him: his FlowFields repo carries
// this separable flow and a full Stam solver (pressure, divergence, vorticity confinement) as
// sibling flows behind one dispatch table. So "no fluid mechanics" describes THIS flow, not his
// work.
//
// Four pieces, each a power function:
//
//   - `inoise16` sampled along one axis fills the two profiles, so the flow drifts and reverses on
//     its own clock rather than blowing steadily in one direction.
//   - `draw::advect16` carries the plane along that flow, backward-sampled and bilinear, which is
//     what smears a dot into a ribbon instead of teleporting it.
//   - `draw::decay16` fades by a half-life, so a trail is the same length in seconds on any device.
//   - `OscillatorBank` walks the emitters, so what is poured in keeps moving.
//
// The plane is 16-bit for the reason every transport effect here is: a value multiplied by slightly
// less than one, many times a second, either dies early or never fades at 8 bits.
//
// Credit: Stefan Petrick, whose concept this is (Jeff credits him for the emitter/flow split too),
// and Jeff (mindful_stone / 4wheeljive), whose ColorTrails is the composition it follows, by way of
// MoonLight. The work has since moved out of AuroraPortal, where the file this was ported from no
// longer exists, into its own repo:
// https://github.com/4wheeljive/FlowFields/blob/main/src/flows/flow_noise.h
// (the version ported here is AuroraPortal at 91ce0c6861, src/programs/colorTrails_detail.hpp).
// The separable-noise-advection idea is theirs; the kernels underneath are this project's.
//
// Two differences from his, deliberate but worth knowing if output is ever compared. His advection
// is TWO passes over a temporary: the second samples the already-x-sheared buffer, so the y-shear
// reads a displaced row. Ours fuses them, reading both offsets from the undisplaced source, which
// is sequential composition against simultaneous: visually close, a different operator. And he
// floors the per-line shift at 0.3 to "prevent pure cardinal motion", because a profile passing
// through zero collapses the flow to a plain horizontal or vertical slide; a low `flow` here can
// reach that state.
// @card ColorTrailsEffect.png
/// Effect: color emitters carried by a flow made of two noise profiles, one per axis.
class ColorTrailsEffect : public EffectBase {
public:
    const char* tags() const override { return "💫🖌️💨🌫️"; }
    Dim dimensions() const override { return Dim::D3; }   // the profiles steer every slice

    /// Which emitters pour color in. They are compositional rather than exclusive: `All` is the
    /// intended picture and the others exist to see one at a time.
    enum class Mode : uint8_t { All = 0, Orbital, Lissajous, Border };

    uint8_t speed       = 60;   // how fast the emitters travel
    uint8_t flow        = 128;  // how far a row or column is pushed: the shear amount
    uint8_t flowSpeed   = 128;  // how fast the profiles themselves drift
    uint8_t scale       = 85;   // the profiles' spatial frequency: few broad bands or many fine
    uint8_t persistence = 60;   // how long color survives, as a half-life
    uint8_t colorSpeed  = 128;  // how fast the emitters walk the palette
    uint8_t size        = 128;  // orbit radius, and the Lissajous figure's reach
    uint8_t mode        = static_cast<uint8_t>(Mode::All);

    /// Which wind carries the color. `Noise` is the separable two-profile flow this effect is
    /// named for; `Radial` is a current running out from the center (or into it), computed per
    /// pixel from geometry rather than sampled, so it costs no field and no state.
    enum class Flow : uint8_t { Noise = 0, Radial, RadialIn };
    uint8_t flowType = static_cast<uint8_t>(Flow::Noise);

    void defineControls() override {
        controls_.addControl("speed", speed, 0, 255);
        controls_.addControl("flow", flow, 0, 255);
        controls_.addControl("flowSpeed", flowSpeed, 0, 255);
        controls_.addControl("scale", scale, 1, 255);
        controls_.addControl("persistence", persistence, 0, 255);
        controls_.addControl("colorSpeed", colorSpeed, 0, 255);
        controls_.addControl("size", size, 0, 255);
        controls_.addSelect("mode", mode, kModes, 4);
        controls_.addSelect("flowType", flowType, kFlows, 3);
    }

    void prepare() override {
        const size_t needed = static_cast<size_t>(width()) * height() * depth() * 3u;
        plane_.resize(needed);
        scratch_.resize(needed);
        carry_.resize(needed);
        // The profiles are the whole velocity field: one value per column, one per row.
        xProf_.resize(static_cast<size_t>(width()));
        yProf_.resize(static_cast<size_t>(height()));
        if (plane_)   std::memset(plane_.data(), 0, plane_.bytes());
        if (scratch_) std::memset(scratch_.data(), 0, scratch_.bytes());
        // The dither accumulator too: resize KEEPS the old contents at an unchanged size, so a
        // re-prepare would carry the previous configuration's error into the first frames.
        if (carry_)   std::memset(carry_.data(), 0, carry_.bytes());
        started_ = false;
        front_ = true;
    }

    void tick() MM_NONBLOCKING override {
        if (!plane_ || !scratch_ || !xProf_ || !yProf_) return;
        const lengthType w = width(), h = height(), d = depth();
        const uint32_t now = elapsed();
        const uint32_t dt = started_ ? now - lastMs_ : 0u;
        lastMs_ = now;
        started_ = true;

        // Both emitter clocks scale from `speed`, and 0 STOPS them rather than leaving a floor:
        // a speed slider whose slowest setting still moves is not a speed slider. The orbit and
        // the Lissajous run at different rates so they drift in and out of step instead of
        // marching together.
        bank_.set(0, {.rate = static_cast<uint16_t>(speed / 4), .low = 0, .high = 65535,
                      .phaseOffset = 0, .wave = Wave::Saw});
        bank_.set(1, {.rate = static_cast<uint16_t>((speed * 3u) / 22u), .low = 0, .high = 65535,
                      .phaseOffset = 16384, .wave = Wave::Saw});
        // advanceTo() takes an ABSOLUTE timestamp and computes its own delta (math16.h BeatPhase):
        // passing this frame's dt instead makes every delta a few milliseconds of a clock that
        // never advances, so the emitters sit still while the picture flickers between them.
        bank_.advanceTo(now);
        // The palette walk is on the same clock, advanced here rather than read off the wall time:
        // `colorSpeed` sets how fast it moves THROUGH the palette, and `speed` still gates it, so
        // stopping the effect stops the color too.
        huePhase_ += (dt * static_cast<uint32_t>(colorSpeed) * static_cast<uint32_t>(speed)) / 24000u;

        sampleProfiles(w, h, now);

        ScratchBuffer<uint16_t>& src = front_ ? plane_ : scratch_;
        ScratchBuffer<uint16_t>& dst = front_ ? scratch_ : plane_;

        // The two shears, applied as one advection: a light's source is offset horizontally by its
        // ROW's profile and vertically by its COLUMN's. Doing both in one backward sample is what
        // keeps this to a single pass, where the original takes two and a temporary buffer.
        const Flow f = static_cast<Flow>(flowType);
        if (f == Flow::Noise) {
            const int32_t* xp = xProf_.data();
            const int32_t* yp = yProf_.data();
            draw::advect16(dst.data(), src.data(), w, h, d,
                           [xp, yp](lengthType x, lengthType y, lengthType,
                                    draw::pos_t& vx, draw::pos_t& vy) {
                               vx = static_cast<draw::pos_t>(yp[y]);
                               vy = static_cast<draw::pos_t>(xp[x]);
                           }, draw::Edge::Wrap);
        } else {
            // RADIAL. Every cell moves along the line through the center, so the direction is the
            // normalized offset from it and needs no field and no state: dividing dx and dy by the
            // distance is the whole computation. The sign is what makes it a fountain or a drain.
            //
            // The edge is CLAMP rather than Wrap, unlike the noise flow: an outward current that
            // wrapped would pour the rim straight back into the middle and the picture would never
            // clear. Clamping lets it run off the edge, which is what an outward flow looks like.
            const int32_t cx = w / 2, cy = h / 2;
            const int32_t reach = (static_cast<int32_t>(flow) * draw::kSubOne) / 24;
            const int32_t sign = (f == Flow::Radial) ? 1 : -1;
            draw::advect16(dst.data(), src.data(), w, h, d,
                           [cx, cy, reach, sign](lengthType x, lengthType y, lengthType,
                                                 draw::pos_t& vx, draw::pos_t& vy) {
                               const int32_t dx = static_cast<int32_t>(x) - cx;
                               const int32_t dy = static_cast<int32_t>(y) - cy;
                               // isqrt of the squared distance, floored at 1 so the center cell
                               // divides by something: it is the one place the direction is
                               // undefined, and it stays put rather than dividing by zero.
                               const int32_t r = static_cast<int32_t>(isqrt(
                                   static_cast<uint32_t>(dx * dx + dy * dy)));
                               const int32_t rr = r < 1 ? 1 : r;
                               vx = static_cast<draw::pos_t>((dx * reach * sign) / rr);
                               vy = static_cast<draw::pos_t>((dy * reach * sign) / rr);
                           }, draw::Edge::Clamp);
        }
        front_ = !front_;
        ScratchBuffer<uint16_t>& moved = front_ ? plane_ : scratch_;

        draw::decay16(moved.data(), moved.count(), halfLifeMs(), dt);
        emit(moved.data(), w, h, d);
        draw::blit16(canvas(), moved.data(), w, h, d, carry_ ? carry_.data() : nullptr);
    }

private:
    static constexpr const char* kModes[4] = {"All", "Orbital", "Lissajous", "Border"};
    static constexpr const char* kFlows[3] = {"Noise", "Radial out", "Radial in"};

    uint32_t halfLifeMs() const {
        return 20u + static_cast<uint32_t>(persistence) * static_cast<uint32_t>(persistence) / 16u;
    }

    /// Fill the two profiles: one noise value per column and per row, in sub-pixels of shift.
    ///
    /// This is the effect's entire velocity field, and the reason it is cheap: W+H noise samples a
    /// frame rather than W*H. The frequency scales inversely with the count so the flow keeps the
    /// same number of bands across it on any grid, rather than turning to static on a large one.
    void sampleProfiles(lengthType w, lengthType h, uint32_t now) {
        const uint32_t phase = (now * static_cast<uint32_t>(flowSpeed)) / 24u;
        // The shear scales with the grid: a fixed pixel count is a strong flow on a 16-wide panel
        // and an invisible one on a 256-wide, which is the same reasoning `fShift` carries upstream.
        const int32_t reach = (static_cast<int32_t>(flow) * (w < h ? w : h)) / 220;
        // BREATHING. Each axis has its own slow multiplier on the shear, so the flow swells and
        // eases instead of shearing at one rate forever. Jeff's version runs six such channels
        // (his FlowFields modulator bank); two is the smallest form that gets the effect, because
        // what sells it is the axes disagreeing, not the channel count. The rates are deliberately
        // unequal and not a ratio of each other, so the pair never returns to the same phase.
        //
        // The DEPTH is bounded so the flow can breathe without stalling: his `minShift` exists
        // because a profile passing through zero collapses the picture to a plain horizontal or
        // vertical slide, and a multiplier that reaches zero does the same thing. 40% swing around
        // 80% keeps it between 0.4 and 1.2 of the set reach.
        const uint32_t mPhase = (now * static_cast<uint32_t>(flowSpeed)) / 96u;
        const int32_t xBreath = 205 + (static_cast<int32_t>(sin16(static_cast<uint16_t>(mPhase * 7u))) * 102) / 32768;
        const int32_t yBreath = 205 + (static_cast<int32_t>(sin16(static_cast<uint16_t>(mPhase * 5u + 21845u))) * 102) / 32768;
        const int32_t xReach = (reach * xBreath) / 256;
        const int32_t yReach = (reach * yBreath) / 256;
        // The axes are detuned in BOTH space and time, which is Stefan's own tuning rather than a
        // guess: his rig's sliders sit at scale 0.33 against 0.32 and speed -1.73 against -1.72,
        // near-equal but never equal. Equal rates let the two profiles lock into one pattern and
        // the picture slides along a diagonal; unequal ones drift through each other, so the flow
        // keeps rearranging itself. The ratios below are his, in integers: 32/33 and 172/173.
        const uint32_t cells = static_cast<uint32_t>(scale) * 24u;
        const uint32_t cellsY = (cells * 32u) / 33u;
        const uint32_t phaseY = (phase * 172u) / 173u;
        for (lengthType x = 0; x < w; x++) {
            const int32_t n = static_cast<int32_t>(inoise16(static_cast<uint32_t>(x) * cells, phase, 0)) - 32768;
            xProf_[static_cast<size_t>(x)] = (n * xReach * draw::kSubOne) / 32768 / 16;
        }
        for (lengthType y = 0; y < h; y++) {
            // A different offset into the field, so the two axes are decoupled: one field read for
            // both would rise and fall together and slide the whole picture along a diagonal.
            const int32_t n = static_cast<int32_t>(inoise16(static_cast<uint32_t>(y) * cellsY, phaseY, 32768)) - 32768;
            yProf_[static_cast<size_t>(y)] = (n * yReach * draw::kSubOne) / 32768 / 16;
        }
    }

    /// The emitters: what is poured in, drawn after the transport so this frame's color is sharp
    /// and only what came before has been carried.
    void emit(uint16_t* p, lengthType w, lengthType h, lengthType d) {
        const Mode m = static_cast<Mode>(mode);
        // An emitter writes at FULL brightness, every frame. What depends on elapsed time is
        // where it is, not how brightly it burns: a dot moves further between two frames on a slow
        // device and less on a fast one, and the trail it leaves is the same either way because
        // `decay16` fades in real time. Scaling brightness by dt instead is what made this effect
        // nearly black in the preview: at 17k fps the frame delta rounds to zero and the emitters
        // wrote nothing, leaving only the border, which covers every frame, visible at all.
        const uint8_t hue = static_cast<uint8_t>(huePhase_);
        const int32_t half = (w < h ? w : h) / 2;
        const int32_t radius = (half * static_cast<int32_t>(size)) / 300 + 1;
        const lengthType dot = static_cast<lengthType>((w < h ? w : h) / 24 + 1);

        if (m == Mode::All || m == Mode::Orbital) {
            // Three circles on one orbit, spaced a third apart, each a third of the palette along.
            for (uint8_t i = 0; i < 3; i++) {
                const angle16 a = static_cast<angle16>(bank_.phase(0) + i * 21845);
                const lengthType cx = static_cast<lengthType>(w / 2 + (static_cast<int32_t>(cos16(a)) * radius) / 32768);
                const lengthType cy = static_cast<lengthType>(h / 2 + (static_cast<int32_t>(sin16(a)) * radius) / 32768);
                splat(p, w, h, d, cx, cy, dot, static_cast<uint8_t>(hue + i * 85));
            }
        }
        if (m == Mode::All || m == Mode::Lissajous) {
            // A Lissajous point: two sines at a 3:2 ratio, which traces a figure that never closes
            // on itself the way a circle does, so the line it lays down keeps finding new ground.
            const angle16 a = static_cast<angle16>(bank_.phase(1));
            const angle16 b = static_cast<angle16>(bank_.phase(1) * 3u / 2u);
            // `size` scales the figure's reach, the same control the orbit radius reads, so the
            // two emitters grow and shrink together instead of the Lissajous always spanning the
            // whole panel.
            const int32_t reachX = ((w / 2 - 1) * static_cast<int32_t>(size)) / 255;
            const int32_t reachY = ((h / 2 - 1) * static_cast<int32_t>(size)) / 255;
            const lengthType cx = static_cast<lengthType>(w / 2 + (static_cast<int32_t>(sin16(a)) * reachX) / 32768);
            const lengthType cy = static_cast<lengthType>(h / 2 + (static_cast<int32_t>(cos16(b)) * reachY) / 32768);
            splat(p, w, h, d, cx, cy, dot, static_cast<uint8_t>(hue + 128));
        }
        if (m == Mode::All || m == Mode::Border) {
            // The rim, its hue walking around the perimeter: the flow pulls it inward, so the
            // border is a source that feeds the whole picture rather than a frame around it.
            for (lengthType x = 0; x < w; x++) {
                const uint8_t c = static_cast<uint8_t>(hue + (x * 255) / (w > 1 ? w : 1));
                put(p, w, h, d, x, 0, c, 255);
                put(p, w, h, d, x, static_cast<lengthType>(h - 1), c, 255);
            }
            for (lengthType y = 0; y < h; y++) {
                const uint8_t c = static_cast<uint8_t>(hue + 128 + (y * 255) / (h > 1 ? h : 1));
                put(p, w, h, d, 0, y, c, 255);
                put(p, w, h, d, static_cast<lengthType>(w - 1), y, c, 255);
            }
        }
    }

    /// A soft round emitter, brightest at its center.
    void splat(uint16_t* p, lengthType w, lengthType h, lengthType d,
               lengthType cx, lengthType cy, lengthType r, uint8_t index) {
        const int32_t r2 = static_cast<int32_t>(r) * r;
        for (lengthType dy = -r; dy <= r; dy++)
            for (lengthType dx = -r; dx <= r; dx++) {
                const int32_t q = static_cast<int32_t>(dx) * dx + static_cast<int32_t>(dy) * dy;
                if (q > r2) continue;
                const uint8_t bri = static_cast<uint8_t>(255 - (q * 255) / (r2 > 0 ? r2 : 1));
                put(p, w, h, d, static_cast<lengthType>(cx + dx), static_cast<lengthType>(cy + dy), index, bri);
            }
    }

    /// Write one light of the wide plane, in the palette's color, taking the BRIGHTER of what is
    /// there and what is poured in.
    ///
    /// Not a sum: an emitter that keeps covering the same light (the border does, every frame)
    /// accumulates without limit and pins it at white, which no half-life can drain and which gets
    /// worse the faster the device runs. A maximum bounds every light by the color actually poured
    /// in, so the picture stays the palette's rather than turning to paper, and the trail still
    /// reads because decay pulls a light down as soon as the emitter moves off it.
    void put(uint16_t* p, lengthType w, lengthType h, lengthType d,
             lengthType x, lengthType y, uint8_t index, uint8_t bri) {
        if (x < 0 || y < 0 || x >= w || y >= h) return;
        const RGB c = colorFromPalette(*Palettes::active(), index, bri);
        const uint8_t ch[3] = {c.r, c.g, c.b};
        for (lengthType z = 0; z < d; z++) {
            const size_t o = ((static_cast<size_t>(z) * h + y) * w + x) * 3u;
            for (uint8_t k = 0; k < 3; k++) {
                const uint16_t v = static_cast<uint16_t>(static_cast<uint32_t>(ch[k]) << 8);
                if (v > p[o + k]) p[o + k] = v;
            }
        }
    }

    ScratchBuffer<uint16_t> plane_{*this};     ///< the color itself, three samples per light
    ScratchBuffer<uint16_t> scratch_{*this};   ///< advect's destination; the two alternate roles
    ScratchBuffer<uint8_t>  carry_{*this};     ///< the dither's per-channel error
    ScratchBuffer<int32_t>  xProf_{*this};     ///< one shift per column: the vertical flow
    ScratchBuffer<int32_t>  yProf_{*this};     ///< one shift per row: the horizontal flow
    OscillatorBank<2>       bank_;
    bool     front_ = true, started_ = false;
    uint32_t lastMs_ = 0, huePhase_ = 0;
};

}  // namespace mm
