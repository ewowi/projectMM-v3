#pragma once
// Moving heads: a rig of them aimed as one instrument.
//
// A single head sweeping a sine is a demo; a ROW of heads is a show, and what makes it read as one
// is the relationship between them. `formation` is that relationship: the same sweep with a
// per-head phase and direction, so the rig fans, mirrors, chases or crosses. One control changes
// the whole character of the rig without touching speed or range.
//
// Pan and tilt run at different rates on purpose. Two sines at different rates trace a Lissajous
// path, which closes into a loop when the rates share a small ratio and drifts when they do not:
// that drift is what keeps a long show from looking like a metronome.
//
// Everything is written through EffectBase's role setters, which are no-ops on a light with no
// such channel. So the same effect on an LED strip paints the color pattern and moves nothing,
// which is the orthogonality the pipeline is built on: an effect says what it wants, and the
// fixture's own preset decides what can be expressed.
// Author: projectMM original

#include "core/AudioService.h"   // latestFrame: the spectrum the audio-reactive mode reads
#include "core/math16.h"         // sin16, BeatPhase: the sweep clocks
#include "light/effects/EffectBase.h"

namespace mm {

/// Effect: aims a rig of moving heads, with formations and an audio-reactive mode.
/// @card MovingHeadEffect.gif
class MovingHeadEffect : public EffectBase {
public:
    const char* tags() const override { return "💫🎶🎯"; }   // audio-reactive when audioReactive is set
    /// D3: every head is a fixture with its own aim, so the effect places all of them itself.
    ///
    /// Declaring D1 would be smaller, but it is a promise the Layer keeps by EXTRUDING: it writes
    /// the x=0 column and copies it across x, so on a 2D rig every fixture in a row would move
    /// identically and on a 3D rig every slice would repeat. That is right for a color pattern
    /// (a 1D gradient genuinely does expand into columns) and wrong for a rig, where the whole
    /// point is that each head has its own place in the formation. Iterating the buffer is what
    /// buys a 2D truss and a 3D array their own dynamics.
    Dim dimensions() const override { return Dim::D3; }

    /// How the heads relate to each other. The point of the effect: the sweep is the same, the
    /// relationship is what the audience reads.
    enum : uint8_t { kFan = 0, kMirror, kChase, kCross, kUnison, kFormationCount };
    static constexpr const char* kFormationNames[] = {"fan", "mirror", "chase", "cross", "unison"};

    uint8_t formation = kFan;

    /// Sweep rates in BPM, the project's speed unit: 60 = one sweep a second.
    uint8_t panBpm  = 6;
    uint8_t tiltBpm = 9;
    /// How much of the fixture's travel to use. A head at full pan spends much of its sweep
    /// pointing away from the audience, so a band around center is the useful default.
    uint8_t panRange  = 128;
    uint8_t tiltRange = 96;
    /// Where the sweep is centered (128 = the fixture's middle).
    uint8_t panCenter  = 128;
    uint8_t tiltCenter = 128;

    /// The beam itself, on a head that has these wheels. Both are raw fixture bytes rather than a
    /// count of slots: a gobo channel is a range per pattern on most heads and every model splits
    /// it differently, so only the fixture's manual can say what a value selects. Left where they
    /// are on a fixture without the channel, and invisible on a rig that has neither.
    uint8_t gobo   = 0;
    uint8_t rotate = 0;
    /// Change gobo on a bass hit rather than holding one pattern all night, the one thing a static
    /// value cannot do. Idea from MoonLight's Troy1 Move, which rolls a new pattern on a kick and
    /// then refuses to roll again for a few seconds: without that hold a busy track strobes through
    /// patterns too fast to read as anything.
    bool goboOnBeat = false;

    /// Move and light with the music: the beam swings wider as the room gets louder, each head
    /// brightens on its own frequency band, and a beat kicks the whole rig. Silence holds it still,
    /// which is what makes the mode read as reactive rather than merely animated.
    bool audioReactive = false;

    void defineControls() override {
        controls_.addSelect("formation", formation, kFormationNames, kFormationCount);
        controls_.addControl("panBpm", panBpm, 1, 120);
        controls_.addControl("tiltBpm", tiltBpm, 1, 120);
        controls_.addControl("panRange", panRange, 0, 255);
        controls_.addControl("tiltRange", tiltRange, 0, 255);
        controls_.addControl("panCenter", panCenter, 0, 255);
        controls_.addControl("tiltCenter", tiltCenter, 0, 255);
        controls_.addControl("audioReactive", audioReactive);
        // Offered only where a head carries the wheels: on an LED strip, or a head without them,
        // the writes are no-ops and the sliders would be three controls that do nothing.
        controls_.addControl("gobo", gobo, 0, 255);
        controls_.addControl("rotate", rotate, 0, 255);
        controls_.addControl("goboOnBeat", goboOnBeat);
        const bool noBeam = !hasBeam();
        const uint8_t last = controls_.count();
        controls_.setHidden(last - 3, noBeam);   // gobo
        controls_.setHidden(last - 2, noBeam);   // rotate
        controls_.setHidden(last - 1, noBeam);   // goboOnBeat
    }

    void prepare() override {
        pan_ = BeatPhase{};
        tilt_ = BeatPhase{};
        beatDecay_ = 0;
        // The rolled gobo starts from the slider, so a rig comes back on the pattern the user
        // chose rather than whatever a beat left behind before the last restart.
        goboNow_ = gobo;
        goboHold_ = 0;
        beatCount_ = 0;
    }

    void tick() MM_NONBLOCKING override {
        const draw::Canvas cv = canvas();
        const nrOfLightsType n = nrOfLights();
        if (n == 0) return;
        // Read at frame time (the framework's rule): a resize between frames must not be missed.
        const uint32_t w = width() ? width() : 1;
        const uint32_t h = height() ? height() : 1;
        const uint32_t d = depth() ? depth() : 1;

        // Own the background: an effect must not inherit the previous frame's picture.
        draw::fill(cv, RGB{0, 0, 0});

        pan_.advanceTo(elapsed(), panBpm);
        tilt_.advanceTo(elapsed(), tiltBpm);

        // phase(65536) is the angle16 form sin16 takes; truncating to uint16 is the free wrap.
        const uint16_t panPhase  = static_cast<uint16_t>(pan_.phase(65536));
        const uint16_t tiltPhase = static_cast<uint16_t>(tilt_.phase(65536));

        // Audio is read ONCE per frame, not per head: the spectrum is the same for all of them,
        // and a per-head read would be the same work times the rig size.
        const AudioFrame* audio = audioReactive ? AudioService::latestFrame() : nullptr;
        const bool live = audio && audio->levelSmoothed >= kSilence;

        // A beat widens the sweep and flares the color, then decays over ~20 frames. Without the
        // decay a kick is a single-frame flicker that no eye can follow.
        const bool beat = live && audio->level > audio->levelSmoothed + kBeatMargin;
        if (beat) beatDecay_ = 255;
        else if (beatDecay_ > kBeatDecayStep) beatDecay_ = static_cast<uint8_t>(beatDecay_ - kBeatDecayStep);
        else beatDecay_ = 0;

        // A beat rolls a new gobo, but only after a HOLD: a pattern needs a few seconds on screen
        // to read as a pattern, and a four-to-the-floor kick would otherwise change it four times a
        // second into a flicker. `gobo` is the base the roll walks from, so the slider still says
        // which family of patterns the rig is in.
        if (goboOnBeat) {
            if (beat && goboHold_ == 0) {
                // hashInt of a BEAT COUNTER, not a stream RNG: two devices running the same rig
                // must land on the same pattern, and a per-call RNG diverges forever the moment one
                // of them renders an extra frame (math16.h, position-addressable randomness).
                beatCount_++;
                goboNow_ = static_cast<uint8_t>(gobo + (hashInt(beatCount_) & 0xE0));   // 8 coarse slots
                goboHold_ = kGoboHoldFrames;
            } else if (goboHold_ > 0) {
                goboHold_--;
            }
        } else {
            goboNow_ = gobo;      // the slider IS the value when the beat roll is off
            goboHold_ = 0;
        }

        // Loud music opens the beam to its full range, quiet keeps it tight. In silence the rig
        // HOLDS its aim rather than drifting.
        const uint16_t loud = live ? audio->levelSmoothed : 0;
        const uint8_t swingPan  = audioReactive ? scaleToLevel(panRange, loud) : panRange;
        const uint8_t swingTilt = audioReactive ? scaleToLevel(tiltRange, loud) : tiltRange;
        const bool frozen = audioReactive && !live;

        for (nrOfLightsType i = 0; i < n; i++) {
            const uint32_t hx = i % w, hy = (i / w) % h, hz = i / (w * h);
            const Formation f = shape(hx, hy, hz, w, h, d);


            // The beam wheels, written every frame like any other role so a value the user changes
            // takes effect at once. Both are no-ops on a fixture without the channel.
            setGobo(i, goboNow_);
            setRotate(i, rotate);

            // A frozen rig keeps its last aim: writing from a stopped phase would snap it back.
            if (!frozen) {
                const int32_t p = sin16(static_cast<uint16_t>(panPhase + f.phase));
                const int32_t t = sin16(static_cast<uint16_t>(tiltPhase + f.phase));
                setPan(i, axis(p * f.dir, panCenter, boost(swingPan)));
                setTilt(i, axis(t, tiltCenter, boost(swingTilt)));
            }

            // Color: a palette walk across the rig, moving with the sweep so color and motion read
            // as one gesture. With audio on, each head takes its brightness from its OWN frequency
            // band, so the rig ripples with the music instead of pulsing as one block.
            const uint8_t hue = static_cast<uint8_t>((panPhase >> 8) + (f.phase >> 8));
            uint8_t bright = 255;
            if (audioReactive) {
                const uint8_t band = static_cast<uint8_t>((static_cast<uint32_t>(i) * 16u) / (n ? n : 1));
                const uint8_t mag = audio ? audio->bands[band > 15 ? 15 : band] : 0;
                // A floor keeps a head whose own band is quiet visible rather than black, and the
                // beat lifts every head so a kick reads across the whole rig at once.
                const uint16_t lifted = static_cast<uint16_t>(kDimFloor + (mag * 3u) / 4u + beatDecay_ / 4u);
                bright = static_cast<uint8_t>(lifted > 255 ? 255 : lifted);
            }
            // pixel, not splat: a fixture IS one light, so its color belongs wholly to it. splat
            // spreads a sub-pixel point across neighbours, which on a 4-fixture rig leaves every
            // head at a fraction of its color (measured: 9 of 255 at full brightness).
            //
            // The flat index unpacked to its own (x, y, z): the same row-major order the layouts
            // and the light buffer use, so head `i` is painted exactly where head `i` lives on any
            // rig shape, a 1 x N chain as much as a 2D truss or a 3D array.
            draw::pixel(cv, {static_cast<lengthType>(hx),
                             static_cast<lengthType>(hy),
                             static_cast<lengthType>(hz)},
                        colorFromPalette(*Palettes::active(), hue, bright));
        }
    }

private:
    static constexpr uint16_t kSilence       = 8;   // below this the room is quiet, not playing
    static constexpr uint16_t kBeatMargin    = 8;   // raw over smoothed = a transient
    static constexpr uint8_t  kBeatDecayStep = 12;  // ~20 frames from a kick back to rest
    static constexpr uint8_t  kDimFloor      = 40;  // a quiet band still shows its head
    /// How long a rolled gobo stays put, in frames. About two seconds at 60 fps: long enough to
    /// read the pattern, short enough that the rig still answers the music.
    static constexpr uint8_t  kGoboHoldFrames = 120;

    /// One head's place in the formation: a phase offset into the sweep, and a direction.
    struct Formation { uint16_t phase; int32_t dir; };

    /// The formations. Each is a different answer to "how do these heads relate", which is what
    /// turns a row of fixtures into one instrument rather than several doing the same thing.
    ///
    /// Driven by the head's POSITION (x, y, z), not its flat index, so the formation reads the
    /// same on any rig shape. On a 1 x N chain x and z are always 0 and this reduces to the old
    /// index-based behavior; on a 2D truss a mirror splits left-from-right the way it looks on
    /// stage rather than splitting the flat index halfway down the grid.
    Formation shape(uint32_t x, uint32_t y, uint32_t z, uint32_t w, uint32_t h, uint32_t d) const {
        // Distance along the rig, as a fraction of a full sweep. A diagonal walk over all three
        // axes: neighbours differ in every direction, so a 3D array ripples like a volume rather
        // than repeating slice by slice.
        const uint32_t span = w + h + d;
        const uint16_t spread = span > 3
            ? static_cast<uint16_t>((static_cast<uint64_t>(x + y + z) * 65536u) / span) : 0;
        switch (formation) {
            case kMirror:
                // The halves face each other: the left of the rig sweeps toward the right. Split
                // on X (the widest axis of a truss), falling back to Y for a 1 x N chain, so the
                // classic look survives on a single column.
                // Split on the axis the rig actually RUNS along: X for a truss, Y for a 1 x N
                // chain, Z for a rig stacked in depth. A fixed fallback to Y would make a
                // 1 x 1 x N rig mirror on h = 1, where every head lands in the same half and the
                // formation collapses to unison.
                return {0, (w > 1 ? (x < w / 2) : h > 1 ? (y < h / 2) : (z < d / 2)) ? 1 : -1};
            case kChase:
                // A wave travelling across the rig: the same sweep, delayed head by head.
                return {spread, 1};
            case kCross:
                // Alternate heads oppose: a tight scissoring that looks fast at a low BPM. The
                // checkerboard parity of the position, so it scissors along every axis at once.
                return {0, ((x + y + z) & 1u) ? -1 : 1};
            case kUnison:
                // Every head as one. Deliberately plain: it is the reference the others read
                // against, and it is what a single-head rig does anyway.
                return {0, 1};
            case kFan:
            default:
                // A fan: neighbours differ by a fraction of the sweep, so the beams open and close
                // like a hand. Half a chase's spread, which keeps the shape readable.
                return {static_cast<uint16_t>(spread / 2), 1};
        }
    }

    /// Widen the sweep on a beat, by up to half again. The beat is the loudest thing in the mode,
    /// so it moves the rig as well as lighting it.
    uint8_t boost(uint8_t range) const {
        if (!audioReactive || beatDecay_ == 0) return range;
        const uint16_t wider = static_cast<uint16_t>(range + (range * beatDecay_) / 512u);
        return static_cast<uint8_t>(wider > 255 ? 255 : wider);
    }

    /// Scale a range by the room's loudness, with a floor so a quiet passage still moves.
    static uint8_t scaleToLevel(uint8_t range, uint16_t level) {
        const uint16_t l = level > 255 ? 255 : level;
        const uint16_t scaled = static_cast<uint16_t>(range / 4u + (range * l) / 340u);
        return static_cast<uint8_t>(scaled > range ? range : scaled);
    }

    /// Map a sine (-32768..32767) onto a DMX byte around `center`, using `range` of the travel.
    /// Clamped: a center near an end plus a wide range must not wrap the axis, which on a real
    /// fixture is a full-speed swing to the opposite stop.
    static uint8_t axis(int32_t wave, uint8_t center, uint8_t range) {
        const int32_t swing = (wave * range) / 65536;
        int32_t v = static_cast<int32_t>(center) + swing;
        if (v < 0) v = 0;
        if (v > 255) v = 255;
        return static_cast<uint8_t>(v);
    }

    BeatPhase pan_;
    BeatPhase tilt_;
    uint8_t   beatDecay_ = 0;
    /// The gobo actually written, which is the slider until a beat rolls it. Held separate so the
    /// slider keeps saying what the user chose rather than being overwritten by the roll.
    uint8_t   goboNow_   = 0;
    uint8_t   goboHold_  = 0;   // frames left before another beat may roll again
    uint32_t  beatCount_ = 0;   // hashed for the roll: shared by every device on the same audio
};

} // namespace mm
