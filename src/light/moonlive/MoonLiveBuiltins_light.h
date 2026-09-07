#pragma once

#include <cstdio>

#include "core/moonlive/MoonLiveBuiltins.h"
#include "core/moonlive/MoonLiveBuiltins_common.h"   // the neutral half: math, waveforms, noise, print
#include "core/moonlive/MoonLive.h"   // runDefineControls drives the engine
#include "core/moonlive/MoonLiveIr.h"   // kArg3: the register `t` is passed in

#include <atomic>
#include <cstdint>

#include "core/math8.h"    // beatsin16: the shared time vocabulary
#include "core/math16.h"   // beat16 / triwave16: full-range waveforms
#include "light/shader.h"  // shader::smoothstep, the GLSL vocabulary, already in fixed point
#include "core/noise.h"    // inoise8: the shared gradient-noise field
#include <cstring>
#include "core/AudioService.h"   // the audio vocabulary reads the latest frame
#include "light/draw.h"    // draw::line, the shared 3D Bresenham a script draws with
#include "light/particles.h" // particles::Pool, the kernel a scripted particle effect drives

// MoonLive: the LIGHT-DOMAIN built-in registration. This is the only place the LED vocabulary
// lives: the function NAMES (`setRGB`, `fill`, `random16`), their arg counts, and the meaning
// of the inline opcodes (StoreElem = an RGB pixel write, FillElems = fill every light). The core
// compiler sees only the neutral BuiltinTable / InlineOp tags this file hands it. A different
// host (display, sensor) would write its own registration with its own names; the core is
// unchanged. (The ESPLiveScript / ARTI bound-function model, doc §3.4.)

namespace mm::moonlive {

// random16(n) → a pseudo-random value in [0, n). A simple LCG, deterministic enough that the
// runtime Bounds guard always sees an in-range index; the same implementation on every target
// so a script behaves identically. The one host helper exposed as a Call so far.
// The palette, as THREE builtins: `paletteR(i, bri)`, `paletteG(i, bri)`, `paletteB(i, bri)`.
// `bri` is the brightness colorFromPalette already takes, and it is what gives a shape a
// radial falloff instead of a flat fill. A builtin returns
// one uint32_t, so a packed 0xRRGGBB would need the script to unpack it, and the language has no
// division or shift to do that with. Three calls is the shape that works today, and
// `setRGB(idx, paletteR(i), paletteG(i), paletteB(i))` reads clearly.
//
// A value a builtin takes as SIGNED: the script's own 32-bit two's complement, read as itself.
//

// A value a builtin takes as a BYTE: clamped to 0..255, not truncated to its low eight bits.
//
// `static_cast<uint8_t>` was the obvious spelling and it is the wrong one. A script computing a
// brightness as `n * 255` means "full", and truncation turns that into an arbitrary walk: n=50
// gives 206, n=128 gives 128, so the brightness draws its own pattern across the picture while
// every part of the expression looks right. Saturating is what the author meant in every case,
// and it is what a hardware byte channel does.
//
// Signed on the way in, so a value that went below zero clamps to 0 rather than to whatever its
// low byte holds.
//
// The boundary: this covers the CALL builtins (setPaletteColor, paletteR/G/B). setRGB and fill
// are inline stores whose channel bytes truncate in the emitted code itself, where a clamp would
// cost three compares per channel per light on the hottest path there is.
inline uint8_t byteArg(uintptr_t a) {
    const int32_t v = signedArg(a);   // one home for the signed reinterpretation of the ABI word
    return v < 0 ? 0 : (v > 255 ? 255 : static_cast<uint8_t>(v));
}

// The ACTIVE palette, so a script follows the device's palette control exactly as a compiled
// effect does, which is the whole point: before this, a script could only hard-code color.
//
// Deliberately NO hsv() alongside it. A hue wheel is how an effect picks color while IGNORING
// the user's palette, which is the habit the compiled effects were moved off (47 of 52 read the
// palette; the exceptions are effects where color carries meaning, like the axis-identifying
// red/green/blue in LinesEffect). Giving scripts hsv() would reintroduce it as the easy default.
extern "C" inline uint32_t mm_light_paletteR(const uintptr_t* args, uint32_t, const uint8_t*) {
    return colorFromPalette(*Palettes::active(), byteArg(args[0]), byteArg(args[1])).r;
}
extern "C" inline uint32_t mm_light_paletteG(const uintptr_t* args, uint32_t, const uint8_t*) {
    return colorFromPalette(*Palettes::active(), byteArg(args[0]), byteArg(args[1])).g;
}
extern "C" inline uint32_t mm_light_paletteB(const uintptr_t* args, uint32_t, const uint8_t*) {
    return colorFromPalette(*Palettes::active(), byteArg(args[0]), byteArg(args[1])).b;
}






// smoothstep(edge0, edge1, v) → a soft 0..65535 ramp between the edges, GLSL's own and the
// anti-aliasing workhorse: wherever a script would draw a hard jaggy edge with an `if`, running
// the distance through this softens it over a width the script picks. `smoothstep(0, w, w - d)`
// turns a distance into a falloff, which is the difference between a stamp and a light source.
//
// A builtin rather than script arithmetic even though '/' now exists: the cubic is two divides
// and three multiplies, so this folds about five host calls into one on a path that runs per
// pixel. That is the bar a builtin has to clear now that the operator covers the general case.
//
// ALL THREE arguments are signed and re-centered here, exactly as polarA/polarR do: a script's
// arithmetic is unsigned, so the natural `w - d` arrives as a huge value once d passes w. Without
// this the outside of a shape reads as fully-inside and the effect renders inverted, a bug that
// looks like a working effect until the shape moves.
extern "C" inline uint32_t mm_light_smoothstep(const uintptr_t* args, uint32_t, const uint8_t*) {
    return shader::smoothstep(signedArg(args[0]), signedArg(args[1]), signedArg(args[2]));
}

// uvX(px, w, h) / uvY(py, w, h) → SHADER SPACE: the pixel's position centered on the grid and
// scaled so the SHORT side spans one unit either way, biased at 32768 like sin/cos so 32768 is
// the origin. This is the mapping every shader starts from, and skipping it is why a design
// STRETCHES on a non-square panel: on a 48x256 wall `x - width / 2` draws a 5:1 ellipse where the
// author wrote a circle.
//
// TWO builtins because a Call returns one value, the shape paletteR/G/B already established.
//
// Scaled so one unit is 8192, not 32768, and biased at 32768. The bias matches sin/cos so
// `uvX(...) - 32768` feeds polarR/polarA with no adapter, and the coarser unit is what leaves the
// LONG axis room: normalization is on the SHORT side, so on a 48x256 wall the long axis reaches
// ±5.3 units and a window that put one unit at 32768 would clip everything past the middle
// sixth: flattening exactly the panel shape uv exists to preserve. At 8192 the range covers a
// 16:1 fixture, and beyond that the value saturates rather than wrapping to the opposite edge.
//
// ONE axis per call, rather than shader::uv's pair with the other half discarded: that computed two
// divides per call and a script asking for both paid four per pixel, on the path the builtin exists
// to make cheap. The 8192 factor is applied before the divide instead of as a shift after it, so
// there is one rounding step rather than two.
//
// The arithmetic is 64-bit and the inputs are read UNSIGNED, because a script's values are unsigned
// 32-bit and `65535 * 65535` is an expression it can write. Read as int32_t that is a large
// NEGATIVE number, so a coordinate far off the right of the grid clamped to the LEFT edge, and
// `px * 2` overflowed a signed 32-bit multiply on the way there. Widening costs one instruction on
// a path already doing a divide, and it is what makes the saturation below honest.
extern "C" inline uint32_t mm_light_uvAxis(const uintptr_t* args, bool wantY) {
    const int64_t px = static_cast<int64_t>(uint32_t(args[0]));
    const int64_t w  = static_cast<int64_t>(uint32_t(args[1]));
    const int64_t h  = static_cast<int64_t>(uint32_t(args[2]));
    const int64_t sw = w < 1 ? 1 : w, sh = h < 1 ? 1 : h;
    const int64_t s  = sw < sh ? sw : sh;          // normalize on the SHORT side: that is what
                                                   // keeps a circle circular on a wide panel
    const int64_t extent = wantY ? sh : sw;
    // Q16.16: 65536 is 1.0, the scale every `fixed` value in the language uses. Applied before the
    // divide rather than as a shift after it, so there is one rounding step rather than two.
    const int64_t v = ((px * 2 - extent + 1) * 65536) / s;
    // ±4.0, which is well past the ±1 the short side normalizes to: a wide panel's long axis runs
    // past 1.0 by its aspect ratio, and 4x covers any panel anyone builds.
    const int64_t c = v < -262144 ? -262144 : (v > 262144 ? 262144 : v);
    // SIGNED and FIXED, with no bias. A coordinate has an origin: the center of the grid is 0.0,
    // the left half is negative, and a script holds the result in a `fixed` member and does
    // ordinary arithmetic on it. The bias this used to add made every consumer write
    // `uvX(...) - 32768`, and that subtraction is exactly what unsigned arithmetic broke: on the
    // left half it wrapped to about 4.29 billion and tore the plane into blocks.
    //
    // sin/cos/beat KEEP their unsigned 0..65535 convention, deliberately: a wave has no origin,
    // and `scale(sin(a), width)` sweeping a full axis is the idiom 14 shipped call sites rely on.
    // A coordinate has an origin, a wave does not.
    return static_cast<uint32_t>(static_cast<int32_t>(c));
}
extern "C" inline uint32_t mm_light_uvX(const uintptr_t* args, uint32_t, const uint8_t*) {
    return mm_light_uvAxis(args, false);
}
extern "C" inline uint32_t mm_light_uvY(const uintptr_t* args, uint32_t, const uint8_t*) {
    return mm_light_uvAxis(args, true);
}

// escape(cx, cy, jx, jy, iters) → how many steps z = z*z + c survives before it runs away,
// scaled to 0..255. The Mandelbrot set when the seed is zero, a Julia set when it is not.
//
// A BUILTIN rather than script arithmetic, and this is the one case where that is not a
// judgement call. The iteration squares a SIGNED fixed-point value, and a script's arithmetic is
// unsigned 32-bit: `x * x` where x holds the wrapped form of -1 computes 65535 * 65535, not 1.
// There is no spelling of this loop in the language, at any cost, until signed values land
// (moonlive-language-roadmap #7). Everything else here stays expressible in script on purpose.
//
// Q16.16, the language's `fixed`: 1.0 is 65536, matching uvX/uvY. A script hands over uv
// coordinates directly, holds them in `fixed` members, and does ordinary arithmetic on them with
// no rescaling anywhere.
//
// The products are int64 and have to be. z*z at the escape radius is 4.0, whose Q32 square is
// about 7.4e10: an int32 overflows there and the point reads as escaped when it has not, which
// draws holes in the middle of the set.
//
// `iters` is the detail dial and the cost: the loop is bounded by it, so a script trades
// definition against frame time directly. Capped at 64, which is where the returned byte stops
// gaining visible bands on a panel, and it bounds the per-pixel cost no matter what a slider says.
extern "C" inline uint32_t mm_light_escape(const uintptr_t* args, uint32_t, const uint8_t*) {
    // Inputs clamped to |8.0| in Q16.16. A coordinate that far out is already deep outside the
    // escape radius (2.0) and iterates identically after clamping; without the clamp, a script
    // passing a full int32 makes zx * zx reach 2^62 and the escape test's SUM overflow int64,
    // which is UB. The clamp is what makes every product below safely wide.
    const auto qfx = [](uintptr_t a) {
        const int32_t v = signedArg(a);
        return v < -524288 ? -524288 : (v > 524288 ? 524288 : v);
    };
    const int32_t cx = qfx(args[0]), cy = qfx(args[1]);
    const int32_t jx = qfx(args[2]), jy = qfx(args[3]);
    uint32_t iters = uint32_t(args[4]);
    if (iters > 64) iters = 64;
    if (iters == 0) return 0;

    // Julia iterates z from the pixel with a FIXED c; Mandelbrot iterates z from zero with c
    // taken from the pixel. One loop serves both: a zero seed selects Mandelbrot, which is why
    // the seed is not a separate builtin.
    const bool julia = (jx != 0 || jy != 0);
    int64_t zx = julia ? cx : 0, zy = julia ? cy : 0;
    const int64_t ax = julia ? jx : cx, ay = julia ? jy : cy;

    constexpr int kShift = 16;
    constexpr int64_t kEscape = int64_t(4) << (kShift * 2);   // |z|^2 > 4.0, in Q32

    uint32_t n = 0;
    for (; n < iters; ++n) {
        const int64_t xx = zx * zx, yy = zy * zy;
        if (xx + yy > kEscape) break;
        const int64_t nx = ((xx - yy) >> kShift) + ax;
        zy = ((2 * zx * zy) >> kShift) + ay;
        zx = nx;
    }
    // Inside the set returns 0, so a script can test for it. Outside, the count spreads over the
    // full byte whatever `iters` is, which keeps the palette mapping independent of the dial.
    return (n >= iters) ? 0u : (n * 255u) / iters;
}




// scale(value, n) → map a 0..65535 value onto 0..n-1. The other half of `beat`: a beat is full-scale
// by design so it is fixture-independent, and this is what lands it on an actual axis. `beat(30, t)`
// then `scale(…, width)` is the sweep position, which is exactly what LinesEffect computes
// (`beat * n / 65536`): including the detail that it REACHES n-1, where the naive `/ 65535` form
// truncates one short and the last column never lights.
// sin(angle) / cos(angle): the full-turn wave, angle 0..65535 for one revolution.
//
// math16's sin16/cos16 return SIGNED -32768..32767; a script's values are unsigned, so the result
// is biased into 0..65535 with the zero line at 32768. A script that wants a coordinate scales the
// result: `scale(sin(a), width)` sweeps the whole axis, which is the same `scale` a beat uses.
// polarA(dx, dy) / polarR(dx, dy): the POLAR pair (Angle, Radius), for an effect written around distance and
// bearing from a center rather than around x/y. Both take offsets that a script computes as
// `x - cx`, which is unsigned and therefore wraps for a point left of center: the builtins
// re-center it themselves (see below), so a script does not have to reason about the wrap.
//
// polarA() returns an angle16 (65536 = one turn), so it feeds straight into sin()/cos(). polarR()
// returns the true distance, not the octagonal approximation, because a visibly non-circular
// "circle" is exactly what an effect using this would be trying to draw.
extern "C" inline uint32_t mm_light_polarA(const uintptr_t* args, uint32_t, const uint8_t*) {
    return static_cast<uint32_t>(atan16(signedArg(args[1]), signedArg(args[0])));
}
extern "C" inline uint32_t mm_light_polarR(const uintptr_t* args, uint32_t, const uint8_t*) {
    return dist16(signedArg(args[0]), signedArg(args[1]));
}



// fbm(x, y, octaves) → octaves of noise summed at doubling frequency and halving amplitude, 0..255.
// One noise() sample is a smooth blur; this is what turns it into cloud, smoke and terrain, with a
// broad shape and finer structure on it. Coordinates are the same 16.0 fixed point noise() takes.
// `octaves` is the cost knob: each one is another noise sample per pixel.
extern "C" inline uint32_t mm_light_fbm(const uintptr_t* args, uint32_t, const uint8_t*) {
    return fbm8(uint32_t(args[0]), uint32_t(args[1]), static_cast<uint8_t>(uint32_t(args[2])));
}

// warp(x, y, strength) → the field sampled at a point that the field itself displaced, 0..255.
// This is the primitive behind the flowing, marbled look: the field stops reading as a texture laid
// over the grid and starts reading as something moving through it. Three noise samples per call.
extern "C" inline uint32_t mm_light_warp(const uintptr_t* args, uint32_t, const uint8_t*) {
    return warp8(uint32_t(args[0]), uint32_t(args[1]), static_cast<uint16_t>(uint32_t(args[2])), 1);
}

// fbm3(x, y, z, octaves) / warp3(x, y, z, strength) → the volumetric forms of the two field
// compositions. The kernels underneath are the same ones the compiled effects call, and each is its
// 2D form when z is 0, so a script can pass a light's depth unconditionally.
extern "C" inline uint32_t mm_light_fbm3(const uintptr_t* args, uint32_t, const uint8_t*) {
    return fbm8(uint32_t(args[0]), uint32_t(args[1]), uint32_t(args[2]),
                static_cast<uint8_t>(uint32_t(args[3])));
}
extern "C" inline uint32_t mm_light_warp3(const uintptr_t* args, uint32_t, const uint8_t*) {
    return warp8(uint32_t(args[0]), uint32_t(args[1]), uint32_t(args[2]),
                 static_cast<uint16_t>(uint32_t(args[3])), 1);
}

// osc(rate, ms, shape) → a low-frequency oscillator, 0..65535, `rate` cycles per minute at time
// `ms`. Shapes: 0 sine, 1 triangle, 2 sawtooth, 3 square (core/oscillators.h names them).
//
// Every animated quantity in a generative field is one of these, and a composition is several at
// different rates: where a shape sits, how far a coordinate is displaced, how fast a layer turns.
// A pure function of time rather than a stateful bank, because a script has no state between
// frames: two calls with the same rate stay locked together for as long as the device runs, which
// is what the compiled OscillatorBank gives an effect and what a composition needs.
extern "C" inline uint32_t mm_light_osc(const uintptr_t* args, uint32_t, const uint8_t*) {
    const uint32_t rate = uint32_t(args[0]), ms = uint32_t(args[1]), shape = uint32_t(args[2]);
    // No special case for rate 0: the phase is then 0 and each shape's own value at phase 0 is the
    // right answer (a sine sits at its midpoint, a triangle and a sawtooth at their start, a square
    // low). An earlier version returned the midpoint for all four, which held a triangle and a saw
    // at 32768 rather than at 0.
    // The phase, as an angle16: rate cycles per minute means rate * ms / 60000 turns.
    const uint32_t phase = static_cast<uint32_t>((static_cast<uint64_t>(ms) * rate * 65536u) / 60000u);
    const angle16 a = static_cast<angle16>(phase);
    switch (shape) {
        case 1:  return triwave16(a);
        case 2:  return a;
        case 3:  return a < 32768 ? 0u : 65535u;
        default: return static_cast<uint32_t>(sin16(a) + 32768);
    }
}


// print(v) → write one value to the serial log, and return it so `print` can be dropped into an
// expression without changing what it computes (`setXYZ(0, print(x), y, z)` still stores x).
//
// This is the only way to see INSIDE a running script. A script that compiles cleanly and produces
// a black fixture gives no other clue: every part reports success and the result is simply wrong.
// That case cost a long debugging session before this existed.
//
// **Rate-limited, because the call sites are per-light.** A modifier's script runs once per light
// per mapping rebuild: 16,384 times on a 128x128 wall. Printing all of them would flood the serial
// line, stall the render (a UART write blocks) and bury the first values, which are the useful
// ones. So a burst is capped and the rest are counted, not printed: the tail of a flood tells you
// nothing the head did not.


// addLight(x, y, z) → place one light at a position. The call a scripted LAYOUT is built on.
//
// A layout cannot write into a buffer the way an effect does: it does not know how many lights it
// will place until it has placed them, and on a classic ESP32 a 16k-light fixture would need 48 KB
// of coordinate staging: memory that board does not have. So the script CALLS OUT instead, once
// per light, and the host decides what to do with each: count it on the sizing pass, emit it into
// the consumer's sink on the walk. Nothing is stored.
//
// The active sink is set by the binding around each run. Outside a run it is null and a call is
// ignored: a script that reaches addLight from an effect places nothing rather than corrupting
// something.
using AddLightFn = void (*)(void* ctx, uint16_t x, uint16_t y, uint16_t z);

/// PER-THREAD, not one global: the sink belongs to whichever thread is running a script, and more
/// than one does. A layout is asked for its light count and its coordinates from the HTTP task when a
/// control is edited, while the render task walks the same layout for the frame: as one global, one
/// thread cleared the sink while the other was mid-run and the built-in called through a live
/// function pointer with a null context. That is a null dereference on the render core, seen as an
/// intermittent crash while resizing a scripted layout.
///
/// Keyed on platform::currentThreadId() rather than C++ `thread_local`, which is UNUSABLE on the
/// ESP32: the compiler reaches TLS through the THREADPTR special register, and a FreeRTOS task
/// created without TLS has THREADPTR = 0: so the access dereferences a small offset from null and
/// dies inside the exception handler. Measured: EXCVADDR 0xfffffff0, `Double exception` in
/// _xt_context_save, on every scripted LAYOUT (the only binding whose script calls a host function).
/// Reading the task handle costs one load and needs no per-task setup.
///
/// The function and the context are ONE struct so they cannot be observed half-updated. Two slots:
/// the render task and whichever task edits a control are the two that ever run a script at once,
/// and a third would mean a genuinely new concurrency story rather than a bigger table.
struct AddLightSink { AddLightFn fn = nullptr; void* ctx = nullptr; };

/// Where a running `defineControls()` sends each `addControl`. Same shape and same
/// reason as the addLight sink: a builtin has no receiver, so the binding installs one for the
/// duration of the run and the call reaches the engine through it.
///
/// `type` is the width the SCRIPT declared, which is what decides the UI control and how many
/// arena bytes a write touches. It is checked against the member's own type by the compiler, so
/// by the time a call arrives here the two already agree.
using AddControlFn = void (*)(void* ctx, const char* name, uint8_t offset,
                              int32_t lo, int32_t hi, CtrlType type);
struct AddControlSink { AddControlFn fn = nullptr; void* ctx = nullptr; };

/// Where fade(amt) sends its request. The binding forwards it to the LAYER rather than to the
/// buffer, because Layer::tick collects every request into one amount and applies it ONCE per
/// frame before the effects run: N fading effects on a shared layer cost one buffer pass, and the
/// gentlest amount wins so the longest trail survives. A builtin that faded the buffer itself
/// would be N passes AND would fight the other effects sharing that layer.
using FadeFn = void (*)(void* ctx, uint8_t amt);
struct FadeSink { FadeFn fn = nullptr; void* ctx = nullptr; };

/// Where setPan/setTilt send their writes. A motion channel is not at a fixed offset the way a
/// color byte is: WHERE pan lives in a light's bytes comes from the layer's fixture channel map,
/// which the engine has no notion of, so this cannot be an Inline store like setRGB and goes
/// through the binding instead.
///
/// `axis` selects which channel, so one sink and one host function serve both rather than two of
/// everything. A light with no such channel is written by nobody: the binding's setPan is already
/// a no-op there, which is what lets one script run on a moving head and on a plain strip.
/// Where setXYZ sends a modifier's transformed coordinate.
///
/// A Call with a sink rather than the Inline store it used to be, and the reason is width: the
/// inline form wrote three BYTES into the caller's buffer, so `setXYZ(767 - xPos, …)` on a
/// 768-wide wall stored 255 and mirrored the light to the wrong place. A layout's addLight was
/// never affected because it is already a Call taking full-width arguments; this brings setXYZ to
/// the same footing.
using CoordFn = void (*)(void* ctx, uint32_t x, uint32_t y, uint32_t z);
struct CoordSink { CoordFn fn = nullptr; void* ctx = nullptr; };

/// Where setPalEntry writes. A PALETTE script fills sixteen entries once per frame, so the sink
/// carries an index rather than a light: it is a table, not a canvas.
using PalFn = void (*)(void* ctx, uint8_t index, uint8_t r, uint8_t g, uint8_t b);
struct PalSink { PalFn fn = nullptr; void* ctx = nullptr; };

/// The five fixture roles, in the order FixtureChannels declares them. One sink and one host
/// function serve all of them, which is why the enum grows rather than each role adding its own.
enum class MotionAxis : uint8_t { Pan = 0, Tilt = 1, Zoom = 2, Rotate = 3, Gobo = 4 };
using MotionFn = void (*)(void* ctx, MotionAxis axis, uint32_t index, uint8_t value);
struct MotionSink { MotionFn fn = nullptr; void* ctx = nullptr; };

/// Where pool(n) sends its sizing request, and where the per-frame particle builtins find the pool.
/// TWO sinks for one feature, deliberately: sizing ALLOCATES, so it is installed only around the
/// defineControls run (the cold path, once per script edit), while the per-frame calls get a
/// read-only handle installed around each tick. A script calling pool(400) from tick() therefore
/// reaches no sizing sink and gets a no-op returning the live count, which is what keeps allocation
/// off the render path entirely.
using TrailSizeFn = bool (*)(void* ctx, bool want);
struct TrailSizeSink { TrailSizeFn fn = nullptr; void* ctx = nullptr; };

using PoolSizeFn = uint16_t (*)(void* ctx, uint16_t count);
struct PoolSizeSink { PoolSizeFn fn = nullptr; void* ctx = nullptr; };
struct PoolSink { particles::Pool* pool = nullptr; uint32_t scale = particles::FrameTime::kOne; };

/// The trail plane a script advects and decays, and the frame's dt.
///
/// A data handle rather than a function sink, for PoolSink's reason: the script names a flow and a
/// persistence, and the BINDING owns the two planes, their geometry and the ping-pong between them.
/// Framerate independence is the system's property too: `dtMs` arrives here rather than being asked
/// of the script, so a script author cannot get it wrong and a slow frame cannot skip the decay.
struct FlowSink {
    uint16_t* a = nullptr;          ///< one of the two planes, three uint16 per light
    uint16_t* b = nullptr;          ///< the other; which one holds the trail is `front`
    /// Points at the BINDING's own flag, so a script's advect calls flip the owner's state
    /// directly. Copying the flag into the sink and reading it back afterwards would work only for
    /// an even number of swaps, and a script may advect once, twice or not at all.
    bool* front = nullptr;          ///< true: `a` holds the trail; false: `b` does
    const uint32_t* frame = nullptr;   ///< the binding's frame counter, for fieldRate
    lengthType w = 0, h = 0, d = 0;
    uint32_t dtMs = 0;
    uint16_t* live() const { return !front ? nullptr : (*front ? a : b); }
    uint16_t* spare() const { return !front ? nullptr : (*front ? b : a); }
};

namespace detail {
// `owner` is ATOMIC and claimed with compare_exchange: the claim used to be a load then a store,
// so two threads could both see the same slot free and both take it: leaving them sharing one
// sink, which is the very aliasing this table exists to prevent.
//
// The slot is the ONE per-thread home for everything a running script's built-ins reach: the
// addLight sink (a layout run installs it) and the draw canvas (an effect run installs it). A
// second table would repeat the claim/release machinery for the same lifetime.
struct SinkSlot { std::atomic<uintptr_t> owner{0}; AddLightSink sink; draw::Canvas canvas;
                  AddControlSink controls; FadeSink fade; MotionSink motion; CoordSink coord;
                  PalSink pal; PoolSizeSink poolSize; PoolSink pool; FlowSink flow;
                  TrailSizeSink trailSize; };
/// Two slots: the render task and whichever task edits a control are the two that ever run a script
/// at once. A third concurrent runner gets the overflow slot, which holds no sink: so its addLight
/// calls no-op instead of writing through someone else's context.
// constinit at namespace scope, not a function-local static: a local static carries a thread-safe
// initialisation guard, which is a lock, and this is read from the render tick. Constant
// initialisation happens before main, so the accessor is a plain address.
inline constinit SinkSlot gSinkSlots[2]{};
inline SinkSlot* sinkSlots() MM_NONBLOCKING { return gSinkSlots; }
/// PERMANENTLY EMPTY. A third concurrent runner reads this and finds no sink, so its addLight calls
/// no-op: setAddLightSink deliberately never installs here, because a shared sink would let two
/// overflow threads write through each other's context.
inline const AddLightSink& sinkOverflow() { static const AddLightSink s; return s; }
/// The canvas twin of sinkOverflow: data stays null, so a third runner's draw calls no-op.
inline const draw::Canvas& canvasOverflow() { static const draw::Canvas c{}; return c; }

/// The slot this thread owns; with `claim`, take a free one when none is owned yet. Null when
/// unowned and not claiming, or when both slots belong to other threads (the overflow case).
inline SinkSlot* ownedSlot(bool claim) MM_NONBLOCKING {
    const uintptr_t me = platform::currentThreadId();
    SinkSlot* slots = sinkSlots();
    for (uint8_t i = 0; i < 2; i++)
        if (slots[i].owner.load(std::memory_order_acquire) == me) return &slots[i];
    if (!claim) return nullptr;
    for (uint8_t i = 0; i < 2; i++) {
        uintptr_t free = 0;
        if (slots[i].owner.compare_exchange_strong(free, me, std::memory_order_acq_rel,
                                                   std::memory_order_relaxed))
            return &slots[i];
    }
    return nullptr;
}
/// Release only a fully empty slot: the six halves (addLight sink, draw canvas, control sink, fade
/// sink, pool sizing sink, pool handle) detach independently, and a release while any of them is
/// live would hand this thread's context to the next claimer, whose script would then reach a dead
/// engine through it.
inline void releaseIfEmpty(SinkSlot* s) MM_NONBLOCKING {
    // EVERY sink the slot carries, motion and coord included: releasing while one is still
    // installed lets another thread claim the slot and reach a context whose run has ended. Two
    // were missed when motion/coord were added, which is why this reads as a list rather than a
    // pair of checks: a new sink that is not named here reintroduces exactly that bug.
    if (s && !s->sink.fn && !s->sink.ctx && !s->canvas.data && !s->controls.fn && !s->fade.fn &&
        !s->motion.fn && !s->coord.fn && !s->poolSize.fn && !s->pool.pool)
        s->owner.store(0, std::memory_order_release);
}
}  // namespace detail

/// This thread's sink. A READ never claims: a freshly claimed slot is empty by construction, so
/// claiming here could only ever return the empty sink it just made, while pinning a slot that
/// nothing will release (only the detach paths release, and a thread that installed nothing never
/// takes one). A script calling addLight from a binding that installs no sink (a modifier) would
/// hold a slot for the life of its task, and two such tasks would exhaust the table and silently
/// stop every later install. Installing claims; reading does not.
inline const AddLightSink& addLightSink() {
    detail::SinkSlot* s = detail::ownedSlot(false);
    return s ? s->sink : detail::sinkOverflow();
}

/// The control sink for this thread, or an empty one. Reading does not claim a slot, for the same
/// reason addLightSink() does not: a binding that installs nothing must not hold a slot for life.
inline const AddControlSink& addControlSink() {
    detail::SinkSlot* s = detail::ownedSlot(false);
    static constinit AddControlSink none{};
    return s ? s->controls : none;
}

/// The fade sink for this thread, or an empty one. Reading does not claim a slot, for the same
/// reason addLightSink() does not.
inline const FadeSink& fadeSink() MM_NONBLOCKING {
    detail::SinkSlot* s = detail::ownedSlot(false);
    static constinit FadeSink none{};
    return s ? s->fade : none;
}

/// Point fade() at the layer for the duration of one run; nullptr to detach. Installed by the
/// binding in the same bracket as the draw canvas, so a script calling fade from a layout or a
/// modifier reaches no sink and does nothing, exactly as line and setPaletteColor already behave.
inline void setFadeSink(FadeFn fn, void* ctx) MM_NONBLOCKING {
    detail::SinkSlot* s = detail::ownedSlot(fn != nullptr);
    if (!s) return;
    s->fade = {fn, ctx};
    if (!fn) detail::releaseIfEmpty(s);
}

/// This thread's coordinate sink, or an empty one. Reading does not claim a slot.
inline const CoordSink& coordSink() MM_NONBLOCKING {
    detail::SinkSlot* s = detail::ownedSlot(false);
    static constinit CoordSink none{};
    return s ? s->coord : none;
}

/// Point setXYZ at the modifier for one run; nullptr to detach.
inline void setCoordSink(CoordFn fn, void* ctx) MM_NONBLOCKING {
    detail::SinkSlot* s = detail::ownedSlot(fn != nullptr);
    if (!s) return;
    s->coord = {fn, ctx};
    if (!fn) detail::releaseIfEmpty(s);
}

/// This thread's palette sink, or an empty one. Reading does not claim a slot.
inline const PalSink& palSink() MM_NONBLOCKING {
    detail::SinkSlot* s = detail::ownedSlot(false);
    static constinit PalSink none{};
    return s ? s->pal : none;
}

/// Point setPalEntry at the palette binding for one run; nullptr to detach. Installed around a
/// single tick, so a script can only ever write the palette it was invoked to fill.
inline void setPalSink(PalFn fn, void* ctx) MM_NONBLOCKING {
    detail::SinkSlot* s = detail::ownedSlot(fn != nullptr);
    if (!s) return;
    s->pal = {fn, ctx};
    if (!fn) detail::releaseIfEmpty(s);
}

/// This thread's motion sink, or an empty one. Reading does not claim a slot, as fadeSink does not.
inline const MotionSink& motionSink() MM_NONBLOCKING {
    detail::SinkSlot* s = detail::ownedSlot(false);
    static constinit MotionSink none{};
    return s ? s->motion : none;
}

/// Point setPan/setTilt at the effect for the duration of one run; nullptr to detach. Installed in
/// the same bracket as the draw canvas, so a script calling setPan from a layout or a modifier
/// reaches no sink and does nothing.
inline void setMotionSink(MotionFn fn, void* ctx) MM_NONBLOCKING {
    detail::SinkSlot* s = detail::ownedSlot(fn != nullptr);
    if (!s) return;
    s->motion = {fn, ctx};
    if (!fn) detail::releaseIfEmpty(s);
}

/// This thread's pool sizing sink, or an empty one. Reading does not claim a slot.
inline const PoolSizeSink& poolSizeSink() MM_NONBLOCKING {
    detail::SinkSlot* s = detail::ownedSlot(false);
    static constinit PoolSizeSink none{};
    return s ? s->poolSize : none;
}

/// Point pool(n) at the binding that owns the buffers, for the duration of one defineControls()
/// run; nullptr to detach. Installed in the same bracket as the control sink, because sizing a pool
/// and declaring a control are the same moment: after the compile, on the cold path, once per edit.
inline void setPoolSizeSink(PoolSizeFn fn, void* ctx) {
    detail::SinkSlot* s = detail::ownedSlot(fn != nullptr);
    if (!s) return;
    s->poolSize = {fn, ctx};
    if (!fn) detail::releaseIfEmpty(s);
}

/// This thread's live pool, or an empty handle. Reading does not claim a slot.
inline const PoolSink& poolSink() MM_NONBLOCKING {
    detail::SinkSlot* s = detail::ownedSlot(false);
    static constinit PoolSink none{};
    return s ? s->pool : none;
}

/// Point the per-frame particle builtins at the pool for the duration of one run; nullptr to
/// detach. Installed by the binding in the same bracket as the draw canvas, so a script calling
/// step() from a layout or a modifier reaches no pool and does nothing.
inline void setPoolSink(particles::Pool* pool, uint32_t scale) MM_NONBLOCKING {
    detail::SinkSlot* s = detail::ownedSlot(pool != nullptr);
    if (!s) return;
    s->pool = {pool, scale};
    if (!pool) detail::releaseIfEmpty(s);
}

/// Where trail() asks for its plane, during defineControls only (the pool's own shape).
inline const TrailSizeSink& trailSizeSink() MM_NONBLOCKING {
    detail::SinkSlot* s = detail::ownedSlot(false);
    static constinit TrailSizeSink none{};
    return s ? s->trailSize : none;
}

/// Install the trail sizer for one defineControls() run; nullptr to detach.
inline void setTrailSizeSink(TrailSizeFn fn, void* ctx) MM_NONBLOCKING {
    detail::SinkSlot* s = detail::ownedSlot(fn != nullptr);
    if (!s) return;
    s->trailSize = {fn, ctx};
    if (!fn) detail::releaseIfEmpty(s);
}

/// The trail plane this run may advect and decay, or an empty handle when there is none.
inline FlowSink& flowSink() MM_NONBLOCKING {
    detail::SinkSlot* s = detail::ownedSlot(false);
    static constinit FlowSink none{};
    return s ? s->flow : none;
}

/// Hand the flow builtins their planes for one run; a null front detaches. The binding owns the
/// buffers and reads `swapped` afterwards to learn which one now holds the trail.
inline void setFlowSink(const FlowSink& f) MM_NONBLOCKING {
    detail::SinkSlot* s = detail::ownedSlot(f.a != nullptr);
    if (!s) return;
    s->flow = f;
    if (!f.a) detail::releaseIfEmpty(s);
}

/// Point addControl at a consumer for the duration of one defineControls() run; nullptr to detach.
/// False when the two-slot table is full, which the caller must not treat as an installed sink:
/// every addControl would then be a silent no-op and the script would publish no controls at all.
inline bool setAddControlSink(AddControlFn fn, void* ctx) {
    detail::SinkSlot* s = detail::ownedSlot(fn != nullptr);
    if (!s) return false;
    s->controls = {fn, ctx};
    if (!fn) detail::releaseIfEmpty(s);
    return true;
}

/// Point addLight at a consumer for the duration of one run; pass nullptr to detach.
///
/// Detaching RELEASES this thread's slot (unless another half is still live), so two slots are
/// not exhausted by tasks that come and go: an HTTP request lands on whichever worker is free.
inline void setAddLightSink(AddLightFn fn, void* ctx) {
    if (!fn && !ctx) {
        // Clear FIRST, release last: the slot must not look free until the sink is cleared.
        detail::SinkSlot* s = detail::ownedSlot(false);
        if (s) { s->sink = {}; detail::releaseIfEmpty(s); }
        return;   // unowned: the overflow holds no sink to clear (see below)
    }
    // Install ONLY into an owned slot. Writing through addLightSink() would install into the shared
    // overflow sink when both slots are taken, and a second overflow thread would then run through
    // the first one's context: the exact aliasing the two-slot table exists to prevent. A third
    // concurrent runner instead gets no sink at all, so its addLight calls no-op: visibly nothing
    // placed, rather than lights written through another thread's layout.
    detail::SinkSlot* s = detail::ownedSlot(true);
    if (s) s->sink = {fn, ctx};
}

// addControl(name, memberOffset, min, max): the run-time half of declaring a control.
//
// The CONTROL RECORD is built by the compiler, which knows the name span and the member's offset,
// so nothing has to travel through a frame slot into a source buffer that is freed by the time
// this runs. What is left is the call itself, which exists so that a script declares a control the
// way a compiled module does: `defineControls()` is an ordinary function the binding calls after a
// successful compile, and this is an ordinary builtin it calls.
// The one control declaration. What kind of control it becomes is read from the MEMBER'S declared
// type rather than chosen by the call, so a call and a declaration cannot disagree.
inline uint32_t addControlDecl(const uintptr_t* args, CtrlType type) {
    // args: (name, memberOffset, min, max). The name is a pointer into the compiled program's
    // string pool, which outlives the run; the offset is the member's arena byte, which the
    // compiler passed by reference.
    const char* name = reinterpret_cast<const char*>(args[0]);
    const AddControlSink s = addControlSink();
    if (!name || !s.fn || !s.ctx) return 0;      // no binding listening: the call is a no-op
    // The range is an ARBITRARY EXPRESSION, so `addControl("n", n, 0, x * 64)` can compute past
    // what the member's type holds. Truncating would publish a slider whose top silently wraps to
    // a small number; refusing the declaration leaves the control absent, which the user can see.
    const int32_t lo = int32_t(args[2]), hi = int32_t(args[3]);
    const int32_t limit = (type == CtrlType::Byte) ? 255 : (type == CtrlType::Bool) ? 1 : INT32_MAX;
    if (lo > limit || hi > limit) return 0;
    // A byte and a bool are UNSIGNED, and the binding casts the range to uint8_t: a negative low
    // bound became min 251 with max 100, a slider that could reach nothing. The old uintptr_t
    // compare caught this for free (a negative wrapped huge); with a signed range it needs saying.
    if ((type == CtrlType::Byte || type == CtrlType::Bool) && lo < 0) return 0;
    // Same stance for an INVERTED range: with min > max the write path's `v < min || v > max` is
    // true for every value, so the slider would appear and then refuse everything the user does to
    // it. Refusing the declaration leaves it absent, which is visible.
    if (lo > hi) return 0;
    s.fn(s.ctx, name, static_cast<uint8_t>(args[1] & 0xff), lo, hi, type);
    return 0;
}

// The member's own type decides the control; this call only says "surface it, within this range".
// The compiler packs both into args[1]: the low byte is the arena offset, the next the CtrlType.
extern "C" inline uint32_t mm_light_addControl(const uintptr_t* args, uint32_t, const uint8_t*) {
    return addControlDecl(args, static_cast<CtrlType>((args[1] >> 8) & 0xff));
}

extern "C" inline uint32_t mm_light_addLight(const uintptr_t* args, uint32_t, const uint8_t*) {
    const uint32_t x = uint32_t(args[0]), y = uint32_t(args[1]), z = uint32_t(args[2]);
    // Both halves checked: a sink is only ever installed as a pair, but a context of null with a live
    // function is exactly what the crash was, so the guard states the whole precondition.
    const AddLightSink s = addLightSink();
    if (s.fn && s.ctx)
        s.fn(s.ctx, static_cast<uint16_t>(x), static_cast<uint16_t>(y), static_cast<uint16_t>(z));
    return 0;
}

/// The canvas the DRAW builtins render into, valid for the duration of one run(). The binding
/// installs it just before engine.run and detaches (a default Canvas) right after. It lives in
/// the same per-thread slot as the addLight sink, the one home for what a running script's
/// built-ins may reach, because C++ `thread_local` is unusable on the ESP32 (see the sink's
/// comment: THREADPTR is 0 on a task without TLS). A binding that installs nothing (a layout, a
/// modifier) leaves the data pointer null and every draw call no-ops: visibly nothing drawn,
/// never a write through another binding's buffer.
inline const draw::Canvas& drawCanvas() {
    detail::SinkSlot* s = detail::ownedSlot(false);   // a read never claims, see addLightSink
    return s ? s->canvas : detail::canvasOverflow();
}
inline void setDrawCanvas(const draw::Canvas& cv) MM_NONBLOCKING {
    if (!cv.data) {
        // Clear FIRST, release last: the same order the sink detach keeps.
        detail::SinkSlot* s = detail::ownedSlot(false);
        if (s) { s->canvas = {}; detail::releaseIfEmpty(s); }
        return;
    }
    detail::SinkSlot* s = detail::ownedSlot(true);
    if (s) s->canvas = cv;
}

/// setPaletteColor(x, y, index, brightness) → one pixel, colored from the ACTIVE palette.
///
/// One call where a script used to write three: `paletteR/G/B` each returned a single channel, so
/// a palette pixel cost three host calls AND three evaluations of whatever expression produced the
/// brightness: the compiler evaluates each argument independently. Measured on an S3, that was
/// 1451 us flat vs 1940 us with a per-pixel falloff; folding it into one call removes two of the
/// three calls and two of the three brightness computations.
///
/// Takes x/y rather than a flat index so the buffer layout stops leaking into every script: a
/// script was writing `mod(bx + dx, width) + mod(by + dy, height) * width` at every call site.
/// Out-of-range coordinates are dropped, not wrapped: a "negative" coordinate arrives as a huge
/// unsigned value, and wrapping it would paint the wrong edge rather than nothing.
/// setPalEntry(i, r, g, b) - write one of the sixteen active palette entries.
///
/// The PALETTE binding's only output, and the reason a palette can be code rather than data: an
/// entry recomputed every frame can follow audio, drift, or come out of an algorithm, none of which
/// a gradient stop list can express.
///
/// The index is BOUNDED rather than wrapped: a script computing an index from a control could
/// otherwise write a neighboring entry and produce a palette nobody wrote, which reads as an engine
/// fault. Out of range does nothing, which is visible in the picture and blames the script.
extern "C" inline uint32_t mm_light_setPalEntry(const uintptr_t* args, uint32_t, const uint8_t*) {
    const PalSink& s = palSink();
    if (!s.fn) return 0;                          // no palette installed: not a palette script
    const uint32_t i = uint32_t(args[0]);
    if (i >= Palette::kEntries) return 0;
    s.fn(s.ctx, static_cast<uint8_t>(i), byteArg(args[1]), byteArg(args[2]), byteArg(args[3]));
    return 0;
}

/// setPalEntryHSV(i, h, s, v) - the same, in the color space a palette is usually reasoned in.
///
/// A hue sweep is one addition per entry in HSV and a table of magic numbers in RGB, which is why
/// both exist rather than leaving a script to convert. Same name MoonLight uses, so a palette
/// written there reads here.
extern "C" inline uint32_t mm_light_setPalEntryHSV(const uintptr_t* args, uint32_t, const uint8_t*) {
    const PalSink& sink = palSink();
    if (!sink.fn) return 0;
    const uint32_t i = uint32_t(args[0]);
    if (i >= Palette::kEntries) return 0;
    const RGB c = hsvToRgb(byteArg(args[1]), byteArg(args[2]), byteArg(args[3]));
    sink.fn(sink.ctx, static_cast<uint8_t>(i), c.r, c.g, c.b);
    return 0;
}

// setPaletteColorZ(x, y, z, index, bri) - one light in a VOLUME, from the active palette.
//
// A separate name rather than an optional argument on setPaletteColor: a builtin's arity is exact
// (the compiler checks `n != fn->argc`), so an optional one would be a change to the call path for
// a case the fixture already tells apart. On a fixture with no depth z is 0 and the two agree.
extern "C" inline uint32_t mm_light_setPaletteColorZ(const uintptr_t* args, uint32_t, const uint8_t*) {
    const draw::Canvas& cv = drawCanvas();
    if (!cv.data) return 0;                       // no canvas installed (a layout, a modifier)
    const uint32_t x = uint32_t(args[0]), y = uint32_t(args[1]), z = uint32_t(args[2]);
    if (x >= uint32_t(cv.dims.x) || y >= uint32_t(cv.dims.y) || z >= uint32_t(cv.dims.z)) return 0;
    draw::pixel(cv, Coord3D{lengthType(x), lengthType(y), lengthType(z)},
                colorFromPalette(*Palettes::active(), byteArg(args[3]), byteArg(args[4])));
    return 0;
}

extern "C" inline uint32_t mm_light_setPaletteColor(const uintptr_t* args, uint32_t, const uint8_t*) {
    const draw::Canvas& cv = drawCanvas();
    if (!cv.data) return 0;                       // no canvas installed (a layout, a modifier)
    const uint32_t x = uint32_t(args[0]), y = uint32_t(args[1]);
    if (x >= uint32_t(cv.dims.x) || y >= uint32_t(cv.dims.y)) return 0;
    draw::pixel(cv, Coord3D{lengthType(x), lengthType(y), 0},
                colorFromPalette(*Palettes::active(), byteArg(args[2]), byteArg(args[3])));
    return 0;
}

/// fade(amt) → dim every light toward black by amt/255, FastLED's fadeToBlackBy under its own
/// name. The trail primitive: an effect that fades instead of clearing leaves a decaying tail
/// behind whatever it draws, which is what a spark, a comet or a scanner looks like.
///
/// Goes to the LAYER, not to the buffer. Layer::tick collects the requests and applies the
/// gentlest one ONCE per frame before the effects run, so two fading effects on one layer cost one
/// pass rather than two, and the longer trail survives. See Layer::fadeToBlackBy.
///
/// Reaches nothing from a layout or a modifier, where no sink is installed, so the call is a
/// no-op there rather than fading a layer the script is not ticking in.
/// trail(1) asks for the trail plane; trail(0) gives it up. Answers whether one is available.
///
/// The pool's shape, for the pool's reason: the planes are two 16-bit buffers (96 KB on a 20-cube),
/// so they exist only for a script that says it advects, and the ask happens at defineControls
/// where allocation is legal. Called from tick() it REPORTS rather than resizing, so the render
/// path never allocates.
extern "C" inline uint32_t mm_light_trail(const uintptr_t* args, uint32_t, const uint8_t*) {
    const TrailSizeSink& s = trailSizeSink();
    if (!s.fn) return flowSink().live() != nullptr ? 1u : 0u;   // outside defineControls: report
    return s.fn(s.ctx, uint32_t(args[0]) != 0) ? 1u : 0u;
}

// The flow builtins: a script names a wind and a persistence, and the binding owns the planes.
//
// `flowNoise(scale, strength)` and `flowCurl(scale, strength)` each ADVECT the whole trail plane in
// one call, because the alternative (a script loop calling a per-pixel rule) would cross the script
// boundary once per light, which on a 20-cube is 8000 calls a frame. One call, the loop in C++.

/// Should this frame do the expensive work? `fieldRate(n)` answers true once every n frames.
///
/// A script's ONLY practical way to afford a per-pixel loop on a large fixture: crossing the script
/// boundary once per light costs 8000 calls a frame on a cube, and skipping four in five of those is
/// the difference between an effect that runs and one that does not. What a caller puts inside the
/// gate is its own composition; guarding the expensive BIRTH while the flow and the decay run every
/// frame keeps the motion smooth and costs only detail, which is what nebula.mle does.
///
/// The counter lives with the binding, not the script: a script holding its own frame count would
/// have to reason about what a frame is, which is the system's business.
extern "C" inline uint32_t mm_light_fieldRate(const uintptr_t* args, uint32_t, const uint8_t*) {
    FlowSink& f = flowSink();
    if (!f.frame) return 1;                       // no binding: never skip, so a script still works
    const uint32_t n = uint32_t(args[0]);
    if (n <= 1) return 1;
    return (*f.frame % n) == 0 ? 1u : 0u;
}

/// Advect the trail along a noise field: two decoupled samples, one per axis.
extern "C" inline uint32_t mm_light_flowNoise(const uintptr_t* args, uint32_t, const uint8_t*) {
    FlowSink& f = flowSink();
    if (!f.live() || !f.spare()) return 0;
    const uint32_t cells = (uint32_t(args[0]) ? uint32_t(args[0]) : 1u) * 256u;
    const int32_t strength = signedArg(args[1]);
    const uint32_t t = platform::millis();
    draw::advect16(f.spare(), f.live(), f.w, f.h, f.d,
                   [&](lengthType x, lengthType y, lengthType z, draw::pos_t& vx, draw::pos_t& vy) {
                       const uint32_t fx = uint32_t(x) * cells, fy = uint32_t(y) * cells;
                       const uint32_t fz = uint32_t(z) * cells + t / 4u;
                       const int32_t nx = int32_t(inoise16(fx, fy, fz)) - 32768;
                       const int32_t ny = int32_t(inoise16(fx + 0x9E37u, fy + 0x7C15u, fz)) - 32768;
                       // 64-bit: `strength` is a SCRIPT value and so unbounded, and a 32-bit
                       // product wraps rather than saturating. curl16 was widened for this; its
                       // noise sibling needs it more, not less.
                       vx = draw::pos_t((static_cast<int64_t>(nx) * strength) >> 15);
                       vy = draw::pos_t((static_cast<int64_t>(ny) * strength) >> 15);
                   }, draw::Edge::Clamp);
    *f.front = !*f.front;                 // the destination now holds the trail
    return 0;
}

/// Advect the trail along a curl field: the same, but divergence-free, so nothing clumps.
extern "C" inline uint32_t mm_light_flowCurl(const uintptr_t* args, uint32_t, const uint8_t*) {
    FlowSink& f = flowSink();
    if (!f.live() || !f.spare()) return 0;
    const uint32_t cells = (uint32_t(args[0]) ? uint32_t(args[0]) : 1u) * 256u;
    const int32_t strength = signedArg(args[1]);
    const uint32_t t = platform::millis();
    draw::advect16(f.spare(), f.live(), f.w, f.h, f.d,
                   [&](lengthType x, lengthType y, lengthType z, draw::pos_t& vx, draw::pos_t& vy) {
                       int32_t cx = 0, cy = 0;
                       curl16(uint32_t(x) * cells, uint32_t(y) * cells,
                              uint32_t(z) * cells + t / 4u, strength, cx, cy);
                       vx = draw::pos_t(cx);
                       vy = draw::pos_t(cy);
                   }, draw::Edge::Clamp);
    *f.front = !*f.front;
    return 0;
}

/// Dim the trail by a half-life in milliseconds: the tail's length, in seconds rather than frames.
///
/// `trailDecay`, not `decay`: a builtin name is reserved for every script, and `decay` is an
/// ordinary word for a member (`pulse.mle` and `beat-flash.mlp` both declare one). Taking it broke
/// both scripts, which is a real backward-compatibility break for a name this vocabulary does not
/// need. The existing `fade` builtin already pushed authors to `fadeAmt` for the same reason.
extern "C" inline uint32_t mm_light_trailDecay(const uintptr_t* args, uint32_t, const uint8_t*) {
    FlowSink& f = flowSink();
    if (!f.live()) return 0;
    const size_t n = size_t(f.w) * f.h * f.d * 3;
    draw::decay16(f.live(), n, uint32_t(args[0]), f.dtMs);
    return 0;
}

/// Throw light into the trail: a disc at the plane's full width, so the decay has somewhere to go.
/// Takes a palette index rather than a color, as setPaletteColor does.
///
/// `radius` in whole lights, and it is what decides whether a long tail is visible at all. Advection
/// spreads a head bilinearly, so after N frames one light's worth of brightness covers roughly N
/// pixels: a single-pixel head on a 4-second tail arrives at about 1 part in 255 and the panel reads
/// as a faint smear. A disc injects radius^2 times as much for the same tail, which is the knob that
/// makes persistence usable rather than merely long.
extern "C" inline uint32_t mm_light_emitTrail(const uintptr_t* args, uint32_t, const uint8_t*) {
    FlowSink& f = flowSink();
    uint16_t* plane = f.live();
    if (!plane) return 0;
    const int32_t cx = signedArg(args[0]), cy = signedArg(args[1]), cz = signedArg(args[2]);
    const RGB c = colorFromPalette(*Palettes::active(), byteArg(args[3]), byteArg(args[4]));
    const int32_t r = signedArg(args[5]);
    const int32_t rad = r < 0 ? 0 : (r > 32 ? 32 : r);          // a runaway radius is a full-frame loop
    const uint16_t wr = uint16_t((c.r << 8) | c.r);             // widened by repeating the byte, so a
    const uint16_t wg = uint16_t((c.g << 8) | c.g);             // full head is 65535 rather than 65280
    const uint16_t wb = uint16_t((c.b << 8) | c.b);
    for (int32_t dy = -rad; dy <= rad; dy++) {
        for (int32_t dx = -rad; dx <= rad; dx++) {
            if (dx * dx + dy * dy > rad * rad) continue;        // a disc, not a square
            // Widened for the sum: cx and cy come from the SCRIPT, so either may be near
            // INT32_MAX, and adding the radius to that is signed overflow before the bounds test
            // ever runs. Only a coordinate that passed the test is narrowed for indexing.
            const int64_t x = static_cast<int64_t>(cx) + dx, y = static_cast<int64_t>(cy) + dy;
            if (x < 0 || y < 0 || cz < 0 || x >= f.w || y >= f.h || cz >= f.d) continue;
            const size_t off = (size_t(cz) * f.h * f.w + size_t(y) * f.w + size_t(x)) * 3;
            plane[off + 0] = wr;
            plane[off + 1] = wg;
            plane[off + 2] = wb;
        }
    }
    return 0;
}

extern "C" inline uint32_t mm_light_fade(const uintptr_t* args, uint32_t, const uint8_t*) {
    const FadeSink& f = fadeSink();
    if (!f.fn) return 0;
    const uint32_t amt = uint32_t(args[0]);
    f.fn(f.ctx, static_cast<uint8_t>(amt > 255 ? 255 : amt));
    return 0;
}

/// setXYZ(x, y, z) → where this light goes, from a modifier. Full width, unlike the inline store
/// it replaces: a coordinate on a large wall does not fit in a byte.
///
/// A no-op outside a modifier run, where no sink is installed, exactly as fade and setPan are.
extern "C" inline uint32_t mm_light_setXYZ(const uintptr_t* args, uint32_t, const uint8_t*) {
    const CoordSink& c = coordSink();
    if (!c.fn) return 0;
    c.fn(c.ctx, uint32_t(args[0]), uint32_t(args[1]), uint32_t(args[2]));
    return 0;
}

/// The audio vocabulary: what the room sounds like, for a script to paint with.
///
/// Reads AudioService's latest frame, the same one every compiled audio-reactive effect uses, so a
/// script and a compiled effect hear exactly the same thing. Every value is already a small integer
/// (the frame is pre-scaled for this), so a script does integer maths straight off them.
///
/// SILENCE READS ZERO, and that is the contract worth relying on: with no audio module, no
/// microphone, or a quiet room, every one of these returns 0 and an audio-reactive script simply
/// renders nothing rather than failing to compile or drawing garbage. A script can therefore be
/// written once and run on a device that has no audio at all. No null check is needed for that:
/// latestFrame() hands back a constexpr silent frame when no module holds the seat.
extern "C" inline uint32_t mm_light_level(const uintptr_t*, uint32_t, const uint8_t*) {
    return AudioService::latestFrame()->level;
}
extern "C" inline uint32_t mm_light_levelSmooth(const uintptr_t*, uint32_t, const uint8_t*) {
    return AudioService::latestFrame()->levelSmoothed;
}
/// band(i) → one of the 16 log-spaced magnitudes, bass at 0 and treble at 15. An out-of-range index
/// reads 0 rather than wrapping: a script asking for band 20 has a bug, and wrapping would answer it
/// with a plausible number from the wrong end of the spectrum.
extern "C" inline uint32_t mm_light_band(const uintptr_t* args, uint32_t, const uint8_t*) {
    const uint32_t i = uint32_t(args[0]);
    return i < 16 ? AudioService::latestFrame()->bands[i] : 0;
}
extern "C" inline uint32_t mm_light_peakHz(const uintptr_t*, uint32_t, const uint8_t*) {
    return AudioService::latestFrame()->peakHz;
}
/// beat() → 1 on a transient, 0 otherwise. The SAME test the compiled effects use (the raw level
/// rising above its own smoothed average by a margin), so "a beat" means one thing across the
/// project rather than each script inventing a threshold. Silence never beats.
extern "C" inline uint32_t mm_light_onBeat(const uintptr_t*, uint32_t, const uint8_t*) {
    const AudioFrame* a = AudioService::latestFrame();
    constexpr uint16_t kSilence = 8, kBeatMargin = 8;
    if (a->levelSmoothed < kSilence) return 0;
    return a->level > a->levelSmoothed + kBeatMargin ? 1 : 0;
}

/// setPan / setTilt / setZoom / setRotate / setGobo (index, value) → aim and shape one head.
///
/// A NO-OP when the light carries no such channel, which is the property that lets one script run
/// on a moving head and on an LED strip: the strip has no pan channel, nothing is written, and the
/// script just paints color. Never scaled by brightness, unlike color, because dimming a rig must
/// not swing its heads toward 0/0.
///
/// A Call rather than an Inline store, unlike setRGB: where pan lives inside a light's bytes comes
/// from the layer's fixture channel map, and the engine has no notion of one. Motion is written
/// once per HEAD per frame where color is written per pixel, so the per-call cost is not on the
/// same path as setRGB's.
/// The one body all five role builtins share: they differ only in which axis they name, and five
/// copies of this would be five places for the byteArg rule below to rot.
///
/// byteArg, not a raw widen: a script computing an aim below zero (pan - 50 past the end)
/// reinterprets as a huge unsigned here and clamps to 255, slamming the head to the OPPOSITE
/// extreme. byteArg is the one home for the signed reading every other builtin uses.
inline uint32_t motionWrite(MotionAxis axis, const uintptr_t* args) {
    const MotionSink& m = motionSink();
    if (!m.fn) return 0;
    m.fn(m.ctx, axis, uint32_t(args[0]), byteArg(args[1]));
    return 0;
}
extern "C" inline uint32_t mm_light_set_pan(const uintptr_t* args, uint32_t, const uint8_t*) {
    return motionWrite(MotionAxis::Pan, args);
}
extern "C" inline uint32_t mm_light_set_tilt(const uintptr_t* args, uint32_t, const uint8_t*) {
    return motionWrite(MotionAxis::Tilt, args);
}
extern "C" inline uint32_t mm_light_set_zoom(const uintptr_t* args, uint32_t, const uint8_t*) {
    return motionWrite(MotionAxis::Zoom, args);
}
extern "C" inline uint32_t mm_light_set_rotate(const uintptr_t* args, uint32_t, const uint8_t*) {
    return motionWrite(MotionAxis::Rotate, args);
}
extern "C" inline uint32_t mm_light_set_gobo(const uintptr_t* args, uint32_t, const uint8_t*) {
    return motionWrite(MotionAxis::Gobo, args);
}

/// pool(n) → size this script's particle pool to n particles, and report what it actually got.
///
/// Called from defineControls(), which is the one moment that is after the compile, on the cold
/// path, and once per script edit. Anywhere else it is a NO-OP that reports the live count: the
/// sizing sink is installed only around that run, so a script calling pool() every tick allocates
/// nothing, every frame, forever. That is what keeps a malloc off the render path.
///
/// Returning the achieved count is the whole error channel: 0 means the allocation failed, which a
/// script can see and a device with less PSRAM than the author assumed reports honestly.
///
/// A script that never calls pool() allocates nothing at all. There is deliberately no default
/// pool: a default would give every scripted effect on the device particle buffers it never asked
/// for, including the ones drawing shaders.
extern "C" inline uint32_t mm_light_pool(const uintptr_t* args, uint32_t, const uint8_t*) {
    const PoolSizeSink& s = poolSizeSink();
    if (!s.fn) {
        const PoolSink& live = poolSink();   // outside defineControls: report, do not resize
        return live.pool ? live.pool->count : 0u;
    }
    const uint32_t n = uint32_t(args[0]);
    return s.fn(s.ctx, static_cast<uint16_t>(n > 65535u ? 65535u : n));
}

// --- The particle vocabulary -------------------------------------------------------------------
//
// Six calls, and every one is a pass over the WHOLE pool: this is the first script vocabulary whose
// cost scales with the OBJECTS rather than with the grid. A shader touches every light every frame
// (metal.mle: ~14 host calls per pixel, 59.6 ms on an 80x48); a particle script makes about nine
// calls per FRAME and the per-particle work happens inside C++ loops.
//
// Coordinates and speeds are in PIXELS, converted to the kernel's sub-pixel units here: a script
// author thinks in the grid they can see, not in 1/256ths of it.
//
// The frame scale rides on the pool handle, so every call is framerate-independent without the
// script naming time. That is deliberate: it is a property of the system, not a thing an author
// remembers to type.
//
// A call with no pool installed (a layout, a modifier, or a script that never called pool()) does
// nothing, the same degrade `line` and `setPaletteColor` already have.

/// A moving seed for the emitters. angleEmit hashes (index, seed) into an angle and a speed, so a
/// seed that does not change makes every frame throw the identical set of sparks.
///
/// Atomic for the same reason random16 is: the render task and a control-edit task can both be
/// inside a script at once, and a lost update would hand two emissions the same pattern.
inline uint32_t nextEmitSeed() MM_NONBLOCKING {
    static std::atomic<uint32_t> seed{0x9E3779B9u};
    return seed.fetch_add(0x9E3779B9u, std::memory_order_relaxed);
}

/// emit(x, y, angle, speed, n, life, hue) → throw `n` particles from a point.
///
/// Wraps angleEmit, which spreads them across a cone and varies each one's speed, so a fountain, a
/// burst and a spray are the same call with different numbers. The cone and the RNG seed are the
/// binding's: a script that had to pass a seed would either hard-code one (making every device
/// identical) or invent one per frame (making the emission jitter).
extern "C" inline uint32_t mm_light_emit(const uintptr_t* args, uint32_t, const uint8_t*) {
    const PoolSink& p = poolSink();
    if (!p.pool || !p.pool->valid()) return 0;
    p.pool->angleEmit(draw::toSub(static_cast<lengthType>(uint32_t(args[0]))),
                      draw::toSub(static_cast<lengthType>(uint32_t(args[1]))),
                      static_cast<angle16>(uint32_t(args[2])),
                      static_cast<draw::pos_t>(uint32_t(args[3])),
                      /*cone*/ 8192,                       // a 45 degree plume: wide enough to read
                                                           // as a spray, narrow enough to aim
                      static_cast<uint8_t>(uint32_t(args[4])),
                      static_cast<uint16_t>(uint32_t(args[5])),
                      static_cast<uint8_t>(uint32_t(args[6])),
                      // The seed must MOVE, or every frame emits the same n trajectories and the
                      // spray reads as a few fixed streams that stack up rather than a plume. A
                      // per-call counter rather than the clock: two emit() calls in one frame must
                      // not share a pattern either.
                      /*seed*/ nextEmitSeed());
    return 0;
}

/// gravity(g) → pull every live particle down by `g` sub-pixels per reference frame squared.
/// The one force that makes matter read as matter.
extern "C" inline uint32_t mm_light_gravity(const uintptr_t* args, uint32_t, const uint8_t*) {
    const PoolSink& p = poolSink();
    if (!p.pool || !p.pool->valid()) return 0;
    p.pool->gravity(static_cast<draw::pos_t>(uint32_t(args[0])), p.scale);
    return 0;
}

/// drag(k) → bleed speed off every live particle, 0 none and 255 nearly all.
/// The counterweight to gravity: without it a pool under constant force accelerates until it
/// teleports, which is the first thing an author hits.
extern "C" inline uint32_t mm_light_drag(const uintptr_t* args, uint32_t, const uint8_t*) {
    const PoolSink& p = poolSink();
    if (!p.pool || !p.pool->valid()) return 0;
    p.pool->drag(static_cast<uint8_t>(uint32_t(args[0])), p.scale);
    return 0;
}

/// step() → move every live particle by its velocity. The integrator; nothing moves without it.
///
/// Also kills anything that has left the grid, which is NOT a separate call on purpose: a particle
/// outside the fixture draws nothing and holds its slot forever, so leaking them is a bug in every
/// effect rather than a choice an author should have to opt out of.
extern "C" inline uint32_t mm_light_step(const uintptr_t*, uint32_t, const uint8_t*) {
    const PoolSink& p = poolSink();
    if (!p.pool || !p.pool->valid()) return 0;
    const draw::Canvas& cv = drawCanvas();
    p.pool->step(p.scale);
    if (cv.data)
        p.pool->killOutside(draw::toSub(cv.dims.x), draw::toSub(cv.dims.y), draw::toSub(2));
    return 0;
}

/// bounce(e) → reflect every particle off the walls of the grid, keeping `e`/256 of its speed.
/// 256 is a perfect bounce and lower loses energy on every contact, so a ball settles.
///
/// The grid is the canvas, not an argument: a script that had to pass width and height could pass
/// the wrong ones, and a wall the fixture does not have is not a thing an author wants.
extern "C" inline uint32_t mm_light_bounce(const uintptr_t* args, uint32_t, const uint8_t*) {
    const PoolSink& p = poolSink();
    if (!p.pool || !p.pool->valid()) return 0;
    const draw::Canvas& cv = drawCanvas();
    if (!cv.data) return 0;
    // The wall is the LAST VALID PIXEL, not the pixel past the end: bounce() clamps a particle to
    // the coordinate it is given, and toSub(dims.x) is one pixel outside the buffer, so a ball
    // resting against that wall renders nowhere and the pit looks empty.
    p.pool->bounce(draw::toSub(static_cast<lengthType>(cv.dims.x - 1)),
                   draw::toSub(static_cast<lengthType>(cv.dims.y - 1)),
                   static_cast<uint16_t>(uint32_t(args[0])));
    return 0;
}

/// collide(radius) → make particles bounce off EACH OTHER, `radius` being the contact distance in
/// whole pixels. This is what turns a pool of independent sparks into objects that pile up.
///
/// The one call in this vocabulary whose cost is NOT linear: it is an N-body check, so doubling the
/// pool quadruples the work. Measured on the host at 3.2 us for 48 particles against 0.1 us without
/// it, and 53.6 us at 200. An S3 is roughly 20-40x slower, so a few dozen balls is comfortable and
/// a few hundred is not. A script that wants a big pool should not call this.
///
/// Call it BEFORE step(): resolving an overlap after integrating can shove a particle outside the
/// grid the wall pass has already checked.
extern "C" inline uint32_t mm_light_collide(const uintptr_t* args, uint32_t, const uint8_t*) {
    const PoolSink& p = poolSink();
    if (!p.pool || !p.pool->valid()) return 0;
    p.pool->collide(draw::toSub(static_cast<lengthType>(uint32_t(args[0]))),
                    /*restitution*/ 200, nextEmitSeed());
    return 0;
}

/// age(rate) → count down every particle's life; a particle reaching zero frees its slot.
/// Without it the pool fills and emit() silently stops, which is the bug that only shows up after
/// a minute on the bench.
extern "C" inline uint32_t mm_light_age(const uintptr_t* args, uint32_t, const uint8_t*) {
    const PoolSink& p = poolSink();
    if (!p.pool || !p.pool->valid()) return 0;
    p.pool->age(static_cast<uint16_t>(uint32_t(args[0])), p.scale);
    return 0;
}

/// render(maxLife) → draw every live particle, dimmed by how much life it has left.
///
/// Reads the ACTIVE palette, so a scripted particle effect follows the device's palette control
/// with no color work in the script at all. Sub-pixel splatting is what makes slow motion smooth
/// on a coarse grid rather than stepping.
extern "C" inline uint32_t mm_light_render(const uintptr_t* args, uint32_t, const uint8_t*) {
    const PoolSink& p = poolSink();
    if (!p.pool || !p.pool->valid()) return 0;
    const draw::Canvas& cv = drawCanvas();
    if (!cv.data) return 0;                       // no canvas (a layout, a modifier): draw nothing
    p.pool->render(cv, static_cast<uint16_t>(uint32_t(args[0])));
    return 0;
}

/// line(x1, y1, x2, y2, r, g, b) → a straight segment on the effect's canvas, z = 0.
///
/// The first seven-argument builtin, riding the args-array call ABI (every Call builtin receives
/// its script arguments as a memory array, so arity is a data question, not a register one).
/// Endpoints are CLAMPED to the canvas extents before drawing: script arithmetic is unsigned and
/// wraps, so a "negative" coordinate arrives as a huge value, and an unclamped pair would send the
/// Bresenham walker on a billions-of-steps march. The clamp turns that into a segment pinned to
/// the edge, visible and instant.
extern "C" inline uint32_t mm_light_line(const uintptr_t* args, uint32_t, const uint8_t*) {
    const draw::Canvas& cv = drawCanvas();
    if (!cv.data) return 0;
    const auto clampAxis = [](uintptr_t v, lengthType n) -> lengthType {
        return v >= uintptr_t(n) ? lengthType(n > 0 ? n - 1 : 0) : lengthType(v);
    };
    const Coord3D a{clampAxis(args[0], cv.dims.x), clampAxis(args[1], cv.dims.y), 0};
    const Coord3D b{clampAxis(args[2], cv.dims.x), clampAxis(args[3], cv.dims.y), 0};
    draw::line(cv, a, b, RGB{uint8_t(args[4]), uint8_t(args[5]), uint8_t(args[6])});
    return 0;
}

// The light-domain SYSTEM VARIABLES: names the host defines and a script may only read. Reserved,
// so a script cannot declare one and a name means the same thing in every script.
//
// `t` is an argument register (free to read); the rest are arena slots the BINDING writes each
// frame from the layer it renders into. Their offsets are fixed constants above the script's
// control range (see kCtrlBytes): a binding caches these slot pointers, so they must never move.
//
// Adding one is a single line here plus the binding writing its slot.
enum : uint8_t {
    kSysWidth  = kCtrlBytes + 0 * kSysVarBytes,
    kSysHeight = kCtrlBytes + 1 * kSysVarBytes,
    kSysDepth  = kCtrlBytes + 2 * kSysVarBytes,
    kSysX      = kCtrlBytes + 3 * kSysVarBytes,
    kSysY      = kCtrlBytes + 4 * kSysVarBytes,
    kSysZ      = kCtrlBytes + 5 * kSysVarBytes,
};

/// Write one system variable into its arena slot, full width.
///
/// FOUR bytes, matching kSysVarBytes and the LoadCtrl32 the compiler emits to read it. A byte-wide
/// write clamped `width` to 255, so a script looping `for (x = 0; x < width; …)` on a 768-wide wall
/// drew a complete picture into a 255x255 corner and left the rest black.
///
/// Unaligned-safe by construction: the block starts at kCtrlBytes (64) and every slot is four bytes
/// on from it, but the store goes through memcpy rather than a cast so it stays correct if the
/// layout ever changes.
inline void writeSysVarSlot(uint8_t* arenaSlot, uint32_t value) MM_NONBLOCKING {
    if (!arenaSlot) return;
    std::memcpy(arenaSlot, &value, sizeof(value));
}

/// The system variables a light script can read. Each binding registers the names it actually
/// WRITES, so an unwritten name stays unknown rather than reading a silent 0: a script that asks
/// for something its host never supplies gets a compile error naming it, which is the honest answer.
///
/// Registering is also what RESERVES the name: a script cannot declare a control or a loop variable
/// that shadows one. Keeping the lists tight is therefore what leaves `x` and `y` usable as ordinary
/// loop counters in the two bindings that have no coordinate to hand out.
///
/// Adding one is a single line here plus the binding writing its slot.

/// The entry points the light domain calls, by role. A script defines the ones its role needs; the
/// engine looks each up by name in the one emitted block.
///
/// A name is a MOMENT, not a role. The host owns the moments and calls whatever the script defined
/// for each one: `tick` when a frame is rendered, `placeLights` when lights are being placed,
/// `modifyLogical` when one coordinate is folded. An entry a script did not define is simply not
/// called, which is why nothing validates which names belong to which module.
///
/// This is what makes the bindings differ by which moments they OWN rather than by kind, and it is
/// what lets one class serve more than one: an effect that also defines `modifyLogical` gets both,
/// with no feature to add. It also leaves `tick` free to mean something in a layout later without a
/// grammar change. Guarding any of it would be code spent forbidding what a script author is
/// entitled to do, and the cost of a name nothing calls is a function that does not run, which is
/// visible immediately rather than silent.
inline constexpr const char* kEntryTick        = "tick";           // an effect, per frame
// The declaration moment, run once after a successful compile rather than per tick: the same
// place a compiled module's defineControls() sits in its lifecycle.
inline constexpr const char* kEntryDefineControls = "defineControls";
inline constexpr const char* kEntryPlaceLights = "placeLights";  // a layout, placing lights
inline constexpr const char* kEntryModify       = "modifyLogical"; // a modifier, folding one light

/// The system variables EVERY light script can read. One vocabulary for all three roles, rather
/// than a table per role.
///
/// The split that preceded this bought less than it cost. It prevented no mistake (a layout reading
/// `width` got a compile error, which is the same outcome as reading a variable that is always
/// zero) and it created a trap: the tables were different vocabularies rather than nested ones, so
/// a name meant one thing in one role and was RESERVED in another. `disasm.py` compiled against the
/// widest table and therefore refused `grid.mll`, the shipped default layout, with "name is a
/// system variable" -- the tool was blind to the one script most worth inspecting.
///
/// `width`/`height`/`depth` mean the same thing everywhere: the dimensions of the grid. A layout
/// DEFINES them by the coordinates it places; an effect and a modifier READ them. What a binding
/// still decides is which slots it WRITES each frame; reading is uniform.
///
/// The per-light coordinate is `xPos`/`yPos`/`zPos`, not `x`/`y`/`z`. Those are the names an author
/// reaches for as loop counters (`grid.mll` uses both), so reserving them globally would break the
/// most ordinary code there is. Only a modifier is handed a coordinate; elsewhere the slots read 0.
inline SysVarTable lightSysVars() {
    SysVarTable t;
    // Elapsed milliseconds, passed in kArg3 on every run. An argument register, so it costs no
    // instruction and no arena byte.
    t.add({"t", SysVarKind::Arg, kArg3});
    t.add({"width",  SysVarKind::Arena, kSysWidth});
    t.add({"height", SysVarKind::Arena, kSysHeight});
    t.add({"depth",  SysVarKind::Arena, kSysDepth});
    t.add({"xPos",   SysVarKind::Arena, kSysX});
    t.add({"yPos",   SysVarKind::Arena, kSysY});
    t.add({"zPos",   SysVarKind::Arena, kSysZ});
    return t;
}

/// The three roles keep their names as aliases of the one table: a binding says which role it is
/// playing, and every call site reads the same way it always did.
inline SysVarTable layoutSysVars()   { return lightSysVars(); }
inline SysVarTable effectSysVars()   { return lightSysVars(); }
inline SysVarTable modifierSysVars() { return lightSysVars(); }

// The light-domain built-in table the binding injects into the compiler. setRGB and fill are
// Inline (they lower to stores: the hot-path writers, no per-call cost); random16 is a Call.
// The distances are signed and re-centered here. draw::smin already widens its intermediates to 64
// bits, and that is load-bearing: a wrapped smin returns a value larger than BOTH inputs, which
// inverts the blend rather than degrading it.
extern "C" inline uint32_t mm_light_smin(const uintptr_t* args, uint32_t, const uint8_t*) {
    return static_cast<uint32_t>(draw::smin(signedArg(args[0]), signedArg(args[1]),
                                            static_cast<int32_t>(uint32_t(args[2]))));
}

inline const BuiltinTable& lightBuiltins() {
    // Built ONCE, for the reason serviceBuiltins gives: ~2 KB by value, constant after registration,
    // and rebuilt on every prepare sweep including the ones that change nothing.
    static const BuiltinTable table = [] {
        BuiltinTable t;
    // The neutral half first, so a name means the same thing here as in a service.
    addCommonBuiltins(t);
    // smin stays here: it wraps draw::smin, a shape helper, so it is not neutral.
    t.add({"smin", 3, /*returns*/ true, BuiltinKind::Call, &mm_light_smin, {}});
    // setRGB(index, r, g, b)  → write one pixel (bounds-guarded). Inline op StoreElem.
    t.add({"setRGB", 4, /*returns*/ false, BuiltinKind::Inline, nullptr, InlineOp::StoreElem});
    // setXYZ(x, y, z)         → write one POSITION (bounds-guarded). The same StoreElem as setRGB:
    // three values at index * stride, and what differs is the destination the binding hands run()
    // (a color buffer for an effect, a coordinate for a modifier), so one op serves both and the
    // engine stays free of any notion of what the three bytes mean.
    //
    // A different OP from setRGB, not the same one with an argument hidden: StoreFirst writes
    // element 0, which is the whole of what a modifier can do. The two are asked different
    // questions. An effect picks a pixel out of a whole buffer, so its index is the point; a
    // modifier is handed ONE coordinate per call, so there is no index to give.
    t.add({"setXYZ", 3, /*returns*/ false, BuiltinKind::Call, &mm_light_setXYZ, {}});
    // fill(r, g, b)           → write every light. Inline op FillElems.
    t.add({"fill", 3, false, BuiltinKind::Inline, nullptr, InlineOp::FillElems});
    // fade(amt)              → dim every light toward black, FastLED's fadeToBlackBy. The trail
    // primitive, collected by the layer so N fading effects cost one pass. See mm_light_fade.
    t.add({"fade", 1, /*returns*/ false, BuiltinKind::Call, &mm_light_fade, {}});
    // The flow family: each advects the whole plane in one call (see the handlers).
    t.add({"trail", 1, /*returns*/ true, BuiltinKind::Call, &mm_light_trail, {}});
    t.add({"fieldRate", 1, /*returns*/ true, BuiltinKind::Call, &mm_light_fieldRate, {}});
    t.add({"flowNoise", 2, false, BuiltinKind::Call, &mm_light_flowNoise, {}});
    t.add({"flowCurl", 2, false, BuiltinKind::Call, &mm_light_flowCurl, {}});
    t.add({"trailDecay", 1, false, BuiltinKind::Call, &mm_light_trailDecay, {}});
    t.add({"emitTrail", 6, false, BuiltinKind::Call, &mm_light_emitTrail, {}});
    // setPan / setTilt / setZoom / setRotate / setGobo → aim a head. Calls, not Inline stores:
    // the channel offset comes from the layer's fixture map, which the engine cannot see.
    // The audio vocabulary. All return 0 without audio, so a script written for a rig with a
    // microphone still runs on one without: it simply renders nothing rather than failing.
    // level(): the RAW level, which snaps to a transient. levelSmooth(): the averaged one, which
    // swells. A punchy effect wants the first, a glowing one the second.
    // NAMED `audio*`, and the prefix is the point: registering a builtin RESERVES the name, so a
    // plain `level` would stop every script that declares `byte level = 200` from compiling. That
    // is exactly the name an author reaches for, and breaking existing scripts to claim it would be
    // the language taking a word the user had first.
    t.add({"audioLevel", 0, /*returns*/ true, BuiltinKind::Call, &mm_light_level, {}});
    t.add({"audioSmooth", 0, /*returns*/ true, BuiltinKind::Call, &mm_light_levelSmooth, {}});
    t.add({"audioBand", 1, /*returns*/ true, BuiltinKind::Call, &mm_light_band, {}});
    t.add({"audioPeakHz", 0, /*returns*/ true, BuiltinKind::Call, &mm_light_peakHz, {}});
    t.add({"audioBeat", 0, /*returns*/ true, BuiltinKind::Call, &mm_light_onBeat, {}});
    t.add({"setPan", 2, /*returns*/ false, BuiltinKind::Call, &mm_light_set_pan, {}});
    t.add({"setTilt", 2, /*returns*/ false, BuiltinKind::Call, &mm_light_set_tilt, {}});
    // The rest of the fixture roles, same shape and same no-op-where-absent contract. A head that
    // carries a zoom, a gobo wheel or a prism is aimed AND shaped from a script, which is what a
    // moving-head look needs beyond pan and tilt.
    t.add({"setZoom", 2, /*returns*/ false, BuiltinKind::Call, &mm_light_set_zoom, {}});
    t.add({"setRotate", 2, /*returns*/ false, BuiltinKind::Call, &mm_light_set_rotate, {}});
    t.add({"setGobo", 2, /*returns*/ false, BuiltinKind::Call, &mm_light_set_gobo, {}});
    // pool(n)                → size this script's particle pool, from defineControls(). Returns the
    // count actually available, 0 when the allocation failed. See mm_light_pool.
    t.add({"pool", 1, /*returns*/ true, BuiltinKind::Call, &mm_light_pool, {}});
    // The particle vocabulary: whole-pool passes, one call per FRAME rather than per pixel.
    // emit(x, y, angle, speed, n, life, hue) → throw n particles from a point.
    t.add({"emit", 7, /*returns*/ false, BuiltinKind::Call, &mm_light_emit, {}});
    // gravity(g) / drag(k) → the two forces a first particle effect needs.
    t.add({"gravity", 1, /*returns*/ false, BuiltinKind::Call, &mm_light_gravity, {}});
    t.add({"drag", 1, /*returns*/ false, BuiltinKind::Call, &mm_light_drag, {}});
    // step() → integrate, and kill whatever left the grid. age(rate) → count down life.
    t.add({"step", 0, /*returns*/ false, BuiltinKind::Call, &mm_light_step, {}});
    t.add({"age", 1, /*returns*/ false, BuiltinKind::Call, &mm_light_age, {}});
    // bounce(e) → reflect off the grid walls, keeping e/256 of the speed.
    t.add({"bounce", 1, /*returns*/ false, BuiltinKind::Call, &mm_light_bounce, {}});
    // collide(radius) → particles notice each other. NOT linear in pool size; see mm_light_collide.
    t.add({"collide", 1, /*returns*/ false, BuiltinKind::Call, &mm_light_collide, {}});
    // render(maxLife) → draw the pool from the active palette.
    t.add({"render", 1, /*returns*/ false, BuiltinKind::Call, &mm_light_render, {}});
    // smoothstep(e0, e1, v)  → a soft 0..65535 ramp between two edges. Turns a distance into a
    // glow; signed arguments, re-centered like polarA. See mm_light_smoothstep.
    t.add({"smoothstep", 3, /*returns*/ true, BuiltinKind::Call, &mm_light_smoothstep, {}});
    // uvX(x, w, h) / uvY(y, w, h) → shader space, centered and short-side normalized so a circle
    // stays a circle on a wide panel. SIGNED fixed (Q16.16), centered on 0.0, no bias.
    // See mm_light_uvAxis.
    t.add({"uvX", 3, /*returns*/ true, BuiltinKind::Call, &mm_light_uvX, {}, /*byRef*/ 0, /*byStr*/ 0, /*fixedArgs*/ 0, /*fixedReturn*/ true});
    t.add({"uvY", 3, /*returns*/ true, BuiltinKind::Call, &mm_light_uvY, {}, /*byRef*/ 0, /*byStr*/ 0, /*fixedArgs*/ 0, /*fixedReturn*/ true});
    // escape(cx, cy, jx, jy, iters) → the escape-time count for z = z*z + c, 0..255, 0 inside.
    // Mandelbrot with a zero seed, Julia otherwise. The one piece of maths a script cannot
    // express: it squares SIGNED values and script arithmetic is unsigned.
    t.add({"escape", 5, /*returns*/ true, BuiltinKind::Call, &mm_light_escape, {}, /*byRef*/ 0, /*byStr*/ 0, /*fixedArgs*/ 0x0f});
    // polarA(dx, dy) / polarR(dx, dy) → polar from a center. atan16 and dist16 already exist in
    // math16.h. NOT named `angle`/`radius`: a script wants those for its own controls
    // (ring.mll and balls.mle both declare `radius`), and a builtin would shadow them. Exposing
    // them here is what lets a radial effect drop its precomputed lookup table.
    t.add({"polarA", 2, /*returns*/ true, BuiltinKind::Call, &mm_light_polarA, {}});
    t.add({"polarR", 2, /*returns*/ true, BuiltinKind::Call, &mm_light_polarR, {}});
    // fbm(x, y, octaves) / warp(x, y, strength) → the two compositions over noise() that turn one
    // smooth field into cloud and into flow. The compiled effects are written out of these, so a
    // script reaches the same vocabulary rather than summing octaves by hand in the grammar.
    t.add({"fbm", 3, /*returns*/ true, BuiltinKind::Call, &mm_light_fbm, {}});
    t.add({"warp", 3, /*returns*/ true, BuiltinKind::Call, &mm_light_warp, {}});
    // Their 3D forms, so a script samples through a volume rather than repeating one slice.
    t.add({"fbm3", 4, /*returns*/ true, BuiltinKind::Call, &mm_light_fbm3, {}});
    t.add({"warp3", 4, /*returns*/ true, BuiltinKind::Call, &mm_light_warp3, {}});
    // osc(rate, ms, shape) → an LFO, the unit every animated quantity is made of. Stateless, so
    // oscillators sharing a rate hold their relationship for as long as the device runs.
    t.add({"osc", 3, /*returns*/ true, BuiltinKind::Call, &mm_light_osc, {}});
    // addLight(x, y, z)      → place a light. A scripted layout's whole vocabulary.
    t.add({"addLight", 3, /*returns*/ false, BuiltinKind::Call, &mm_light_addLight, {}});
    // line(x1, y1, x2, y2, r, g, b) → a segment on the canvas, via the shared draw::line.
    t.add({"line", 7, /*returns*/ false, BuiltinKind::Call, &mm_light_line, {}});
    // addControl(name, member, min, max) → surface a member in the UI, the same shape a compiled
    // module uses. Bit 1 of byRef marks the second argument as the MEMBER, so the compiler passes
    // its arena offset (and its type) rather than its value, which is what makes the script read
    // as the reference a compiled module passes. ONE call for every type: which widget appears
    // follows from how the member was declared, so the two can no longer disagree.
    t.add({"addControl", 4, /*returns*/ false, BuiltinKind::Call, &mm_light_addControl, {},
           /*byRef*/ 0x2, /*byStr*/ 0x1});
    // setPaletteColor(x, y, i, bri) → one palette-colored pixel. The form a script should reach
    // for: one call, one brightness evaluation, and no buffer-layout arithmetic at the call site.
    t.add({"setPaletteColor", 4, /*returns*/ false, BuiltinKind::Call, &mm_light_setPaletteColor, {}});
    // The volumetric write: the same call with the light's depth, so a script paints a cube rather
    // than its z = 0 slice.
    t.add({"setPaletteColorZ", 5, /*returns*/ false, BuiltinKind::Call, &mm_light_setPaletteColorZ, {}});
    // setPalEntry(i,r,g,b) / setPalEntryHSV(i,h,s,v) -> write one of the sixteen ACTIVE palette
    // entries. A palette script's only output; a no-op in every other role, where no sink is
    // installed, so an effect calling it changes nothing rather than corrupting the palette.
    t.add({"setPalEntry", 4, /*returns*/ false, BuiltinKind::Call, &mm_light_setPalEntry, {}});
    t.add({"setPalEntryHSV", 4, /*returns*/ false, BuiltinKind::Call, &mm_light_setPalEntryHSV, {}});
    // paletteR/G/B(i, bri)    → one channel each, for a script that needs the components. Kept
    // because setPaletteColor writes a pixel and cannot serve a script that wants the value.
    t.add({"paletteR", 2, /*returns*/ true, BuiltinKind::Call, &mm_light_paletteR, {}});
    t.add({"paletteG", 2, /*returns*/ true, BuiltinKind::Call, &mm_light_paletteG, {}});
    t.add({"paletteB", 2, /*returns*/ true, BuiltinKind::Call, &mm_light_paletteB, {}});
    // The table is built once at startup; a dropped registration would surface much later as
    // "unknown function" in a script, so it is caught HERE.
    MM_ASSERT_NO_BUILTIN_OVERFLOW(t);
        return t;
    }();
    return table;
}

/// Run a script's `defineControls()`, so the controls it declares exist.
///
/// A compiled module's controls exist because `defineControls()` RAN: the Scheduler calls it on
/// every module at setup, and again whenever a Select reshapes the visible set. A scripted one
/// works the same way. This calls the entry point, each `addControl` inside it reaches the engine
/// through the control sink, and the binding's `rebuildControls()` then finds a populated list.
///
/// Re-runnable, like its compiled counterpart: the list is cleared first, so calling it twice
/// rebuilds rather than appends. A script that defines no `defineControls()` declares no controls,
/// which is the honest answer for a script that wants no UI.
/// `sizePool` is the binding's pool sizer, or null for a binding with no particles (a layout, a
/// modifier). Installed and detached in the same bracket as the control sink: sizing a pool and
/// declaring a control are the same moment, and sharing the bracket means a script's pool cannot
/// be resized from anywhere else.
inline void runDefineControls(MoonLive& engine, PoolSizeFn sizePool = nullptr, void* poolCtx = nullptr,
                              TrailSizeFn sizeTrail = nullptr, void* trailCtx = nullptr) {
    // A script with no defineControls() declares no controls, which is the honest answer for one
    // that wants no UI: there is nothing to clear and nothing to run.
    if (!engine.hasEntry(kEntryDefineControls)) return;
    // Install BEFORE clearing. With the two-slot table full the sink cannot be installed, and a
    // clear-then-run would drop every control and rebuild none of them: the script would appear to
    // declare nothing. Keeping the previous set is the honest degrade, and the run is skipped
    // rather than executed into a dead sink.
    if (!setAddControlSink([](void* ctx, const char* n, uint8_t off,
                              int32_t lo, int32_t hi, CtrlType type) {
            static_cast<MoonLive*>(ctx)->addDeclaredControl(n, off, lo, hi, type);
        }, &engine)) return;
    if (sizePool) setPoolSizeSink(sizePool, poolCtx);
    if (sizeTrail) setTrailSizeSink(sizeTrail, trailCtx);
    engine.clearDeclaredControls();      // re-runnable: rebuild rather than append
    // A one-light scratch buffer: this entry point writes no pixels, but `run` refuses a null or
    // undersized one, and honoring that contract costs less than carving out an exception.
    uint8_t scratch[3] = {};
    engine.run(scratch, 1, 3, 0, kEntryDefineControls);
    if (sizePool) setPoolSizeSink(nullptr, nullptr);
    if (sizeTrail) setTrailSizeSink(nullptr, nullptr);
    setAddControlSink(nullptr, nullptr);
}

}  // namespace mm::moonlive
