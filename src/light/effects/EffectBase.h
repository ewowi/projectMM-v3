#pragma once

// Include this one file to write an effect: it brings EffectBase, the render context accessors, and the
// common drawing / palette / maths / noise / color / scratch / audio helpers, so a new effect is a single
// include:
//
//   #pragma once
//   #include "light/effects/EffectBase.h"
//   namespace mm {
//   class MyEffect : public EffectBase { ... };
//   }
//
// The helper set is the WHOLE surface available to an effect — the render context, the draw/palette/math/
// noise primitives, scratch memory (ScratchBuffer), the audio spectrum (AudioService/AudioFrame), the crc
// fingerprint, and the <cstring>/<cmath> the bodies use. This is also the surface a scripted MoonLive effect
// gets uniformly, and it costs zero firmware bytes (unused declarations emit no code). An effect that needs a
// helper genuinely OUTSIDE this surface — a font table, the module factory, a network packet format, a
// platform primitive — adds that one extra include; nothing else should appear at the top of an effect header.
//
// Note the layout: the helper includes (chiefly light/layers/Layer.h, which defines EffectBase's accessor
// bodies) are pulled in at the BOTTOM of this file, AFTER the EffectBase class. Layer.h includes EffectBase.h
// (a Layer holds effect children), so EffectBase.h can only forward-declare Layer up here; putting Layer.h
// after the class lets it re-enter (a no-op via the include guard) with EffectBase already complete, so its
// out-of-line accessor definitions compile — the standard forward-declare-then-include-the-definer pattern
// that breaks the cycle.

#include "core/MoonModule.h"
#include "light/light_types.h" // lengthType, nrOfLightsType, Dim

#include <cstdint>

// draw.h is included at the BOTTOM of this header (it needs Layer, which needs EffectBase), so the
// Canvas type is forward-declared here for the canvas() accessor's return type.
namespace mm::draw { struct Canvas; }

namespace mm {

class Layer; // forward declaration (defined in light/layers/Layer.h, included at the bottom)

// Dim enum lives in light/light_types.h so both EffectBase and ModifierBase can
// refer to it without including each other. Used by Layer::extrude to fill unused
// axes (D1 column → x and z; D2 slice → z; D3 native) and by the UI to derive the
// 📏/🟦/🧊 dimensional emoji (so it isn't repeated in each module's tags()).
// ModuleFactory::registerType<T> captures dim from a probe via if-constexpr —
// no per-domain registration wrapper is needed.

/// Light-domain MoonModule subclass for effects — adds the rendering context.
///
/// **Design.** A zero-state convenience layer: it holds no data of its own, just
/// accessors (`buffer()`, `width()`, `height()`, `depth()`, `elapsed()`, …) that
/// forward to the parent Layer. An effect reads its rendering context through these
/// instead of caching a `Layer*` and the dimensions itself. `DriverBase` plays the
/// same role for drivers against the Drivers container.
///
/// **Animation guidelines.** Effects use BPM (beats per minute) for speed controls,
/// not abstract 0–255 ranges, giving users a musical reference (60 BPM = one beat per
/// second, 120 BPM = two; default 60). Animation must be resolution-independent:
/// multiply the time offset by the panel dimension (width or height) so the perceived
/// visual speed is the same regardless of display size — otherwise large displays look
/// sluggish and small ones frantic at the same BPM. Animation is driven by elapsed
/// millis (via `elapsed()`), not frame count, so speed stays consistent regardless of
/// FPS; the speed slider controls the animation dynamics, never the framerate, which
/// should always be maximal for smooth motion.
///
/// **Prior art:** MoonLight's Node + VirtualLayer — effects access
/// `layer->width()/height()/depth()` directly via the VirtualLayer pointer (no separate
/// EffectBase), buffer access via `layer->virtualChannels`, time via `timeMicros()`
/// (https://github.com/ewowi/MoonLight/blob/main/src/MoonBase/Nodes.h,
/// https://github.com/ewowi/MoonLight/blob/main/src/MoonLight/Layers/VirtualLayer.h).
class EffectBase : public MoonModule {
public:
    ModuleRole role() const MM_NONBLOCKING override { return ModuleRole::Effect; }

    /// Which axes the effect *iterates* — a claim, not a guarantee about the layer.
    ///
    /// Default D3 means "I iterate every axis the layer gives me" — the framework
    /// doesn't extrude on your behalf. Override to D2 if you write only the z=0
    /// slice (Layer::extrude duplicates it across z); to D1 if you write only the
    /// x=0,z=0 column (1D runs along Y — extrude fills x and z). Declaring D2/D1 is an opt-in promise:
    /// the framework treats slices you don't write as authoritative copies of the
    /// ones you did. On a 2D layer (depth=1) the D3-vs-D2 distinction is free —
    /// extrude's z-fill loop is guarded by `depth_ > 1` and does nothing. The `dim`
    /// int (1/2/3) is emitted in `/api/types`; the UI derives the 📏/🟦/🧊 chip from
    /// it, so it isn't repeated in each module's `tags()`.
    ///
    /// **Contract — tick() must honour layer dimensions.** `dimensions()` is a
    /// claim about which axes the effect *iterates*, not a guarantee that the
    /// layer has that many axes. A D3 effect may run on a D1 or D2 layer (the
    /// layer just has depth=1 and/or height=1). Your loop must read `width()`,
    /// `height()`, and `depth()` at frame time and iterate exactly those bounds —
    /// never hardcode a maximum, never assume your declared D matches the
    /// layer's. Concretely:
    ///   - A D3 effect on a 1D layer (h=d=1) iterates only x; y and z stay 0.
    ///   - A D2 effect on a 1D layer (h=d=1) iterates only x; y stays 0.
    ///   - A D1 effect on a 3D layer writes its (x=0) column and extrude fills the rest.
    /// Hardcoding a fixed `z < SOMETHING` is a buffer-overrun bug — the buffer
    /// is sized to width × height × depth × channels, no more. Tests in
    /// test_extrude.cpp pin the D3-on-2D and D3-on-1D paths for the shipped
    /// effects; add similar pinning for new D3 effects with z-aware math.
    virtual Dim dimensions() const { return Dim::D3; }

    /// Parent is always a Layer (defined in Layer.h after Layer is complete).
    Layer* layer() const;

    // Convenience accessors — delegate to parent Layer.
    // Defined after Layer is complete (in Layer.h).
    uint8_t* buffer();
    lengthType width() const;
    lengthType height() const;
    lengthType depth() const;
    uint8_t channelsPerLight() const;
    nrOfLightsType nrOfLights() const;
    uint32_t elapsed() const;         ///< Milliseconds since render start — drive animation off this, not frame count.

    /// The layer's surface as one value: buffer, extents and channels bound together, with the
    /// depth guard applied. Replaces the `Coord3D dims{...}` + `Buffer& buf = layer()->buffer()`
    /// preamble 22 effects open with, and the private `depthDim()` helper 16 of them carry.
    ///
    /// Call it once per frame and pass the result to draw calls — the extents are read from the
    /// layer at that moment, which is the contract effects already follow (dimensions can change
    /// between frames, so nothing may cache them across ticks).
    mm::draw::Canvas canvas();

    /// Aim a fixture. `index` is the light, `value` the raw DMX byte for that axis (0..255 over
    /// the fixture's full travel: 540 degrees of pan, 180 of tilt on a typical head).
    ///
    /// Each is a NO-OP when the light carries no such channel, which is what lets one effect run
    /// on a moving head and on an LED strip: the strip has no pan channel, so nothing is written
    /// and the effect just paints color. Never scaled by brightness, unlike color: dimming the rig
    /// must not swing a head toward 0/0.
    void setPan(nrOfLightsType index, uint8_t value);
    void setTilt(nrOfLightsType index, uint8_t value);
    void setZoom(nrOfLightsType index, uint8_t value);
    /// The beam's own two roles, on the fixtures that carry them. `rotate` spins the gobo or the
    /// prism; `gobo` selects the pattern, and on most heads its byte is a RANGE per slot rather
    /// than an index, so a fixture's manual decides what a value means.
    ///
    /// Same contract as the aim setters above: a no-op where the channel is absent, and never
    /// scaled by brightness. They complete the motion set the preset model already carries
    /// (FixtureChannels holds all five), which until now had no way for an effect to reach them.
    void setRotate(nrOfLightsType index, uint8_t value);
    void setGobo(nrOfLightsType index, uint8_t value);

    /// True when the lights carry pan or tilt, so an effect can skip motion maths on a strip.
    bool movable() const;

    /// True when the lights carry a gobo or rotate channel, so an effect can hide beam controls
    /// that would do nothing on this rig. Separate from movable(): plenty of heads pan and tilt
    /// without a gobo wheel, and a static wash can carry one without moving at all.
    bool hasBeam() const;
};

} // namespace mm

// --- The effect author's helper surface (see the file header). Pulled in AFTER the EffectBase class so
// Layer.h — which includes this file — re-enters harmlessly and completes EffectBase's accessor bodies. ---
#include "light/layers/Layer.h"   // EffectBase's out-of-line accessors (layer/buffer/width/height/…)
#include "light/draw.h"           // draw::pixel / fill / line / fade / blur — write pixels by coordinate
#include "light/Palette.h"        // colorFromPalette, Palettes::active — the palette system
#include "core/math8.h"           // beat8 / beatsin8 / sin8 / random8 — the integer animation helpers
#include "core/noise.h"           // inoise8: the shared gradient-noise field
#include "core/color.h"           // RGB
#include "core/crc.h"             // crc16 — grid/state fingerprints (stasis detection)
#include "core/ScratchBuffer.h"   // ScratchBuffer<T> — self-sizing scratch memory for stateful effects
#include "core/oscillators.h"     // OscillatorBank / Wave: the motion kernel five effects share
#include "core/math16.h"          // angle16, kaleido, halfLifeKeep: the fixed-point trig and decay
#include "light/polar.h"          // PolarLut: the per-pixel angle and radius four radial effects read
#include "core/AudioService.h"    // AudioService::latestFrame() — the shared audio source
#include "core/AudioFrame.h"      // AudioFrame — level + 16-band spectrum an audio-reactive effect reads

#include <cstring>                // memset / memcpy / strcmp — buffer + control-name handling
#include <cmath>                  // sqrtf / sinf / log10f: per-frame float math (never per-light)
#include <array>                  // std::array — fixed-size effect state tables (cube faces, LUTs)
