#pragma once

#include "light/light_types.h"   // Coord3D, lengthType
#include "light/layers/Buffer.h" // Buffer (flat light array)
#include "core/color.h"          // RGB, scale8
#include "core/math8.h"
#include "core/math16.h"       // isqrt: true-distance SDFs; halfLifeKeep: the decay weight
#include "light/Palette.h"       // blend(RGB,RGB,amt): for blendPixel
#include "light/fonts.h"         // fonts::Font: bitmap glyph tables for draw::text

#include <algorithm>             // std::reverse: in-place rotation for a wrapping scroll
#include <cstring>               // std::memmove/memcpy/memset: the scroll's run moves

// Geometry draw primitives for effects/modifiers: set a pixel, draw a line: bounds-clipped,
// integer-only, working 1D→3D against the flat light Buffer. The "core absorbs the hard part"
// rule applied to drawing: the Bresenham + clipping lives here once, so an effect calls
// drawLine() instead of re-rolling it. Light-domain (it touches the light Buffer), not core.
//
// Prior art: the line algorithm is Bresenham (1962) generalised to 3D (the textbook DDA-error
// form). FastLED keeps draw in its 2D/matrix add-ons, not core: same split here.
//
// The Buffer is a flat array of `count` lights × `cpl` channels; the grid SHAPE (w,h,d) lives on
// the Layer/Layout, so the caller passes `dims` (the Coord3D extent). Index order matches the
// engine: off = (z·h·w + y·w + x)·cpl. A pixel outside [0,w)×[0,h)×[0,d) is silently clipped, so
// a line that runs off the grid just stops drawing: no out-of-bounds write (the robustness rule).

namespace mm::draw {


/// The surface a draw call writes to: a buffer plus the grid dimensions that address it.
///
/// Today every draw call takes `(Buffer&, Coord3D dims)` as two independent arguments that nothing
/// checks for agreement: pass dims from one layer with a buffer from another, or (far likelier) a
/// dims computed with the depth guard and one without, and the result is silent misaddressing.
/// Binding them into one value makes the mismatch unrepresentable rather than merely detected.
///
/// It also owns the depth guard: `depth()` is 0 on a 2D layer, which would zero the z stride, so
/// SIXTEEN effects each carry a private `depthDim()` helper. Constructing a Canvas applies it once.
///
/// **Passed BY VALUE, deliberately.** It is a small POD (pointer + 3 int16 + 2 small ints) and
/// measured 62 instructions in a per-pixel fill loop against 67 for today's separate arguments and
/// 69 for a `const Canvas&`: a reference member forces the extents to be re-read from memory
/// because the compiler must assume they alias the buffer being written, while a by-value POD stays
/// in registers. So the abstraction is not a cost here; it is a small win.
struct Canvas {
    uint8_t* data = nullptr;   ///< first byte of the light array
    size_t   bytes = 0;        ///< total writable bytes (the bound every write is clipped to)
    Coord3D  dims{0, 0, 0};    ///< logical extents; z is >= 1 (see the depth guard above)
    uint8_t  cpl = 3;          ///< channels per light

    /// Build from a Buffer + the layer's logical extents, applying the depth guard.
    static Canvas of(Buffer& buf, lengthType w, lengthType h, lengthType d) {
        return Canvas{buf.data(), buf.bytes(),
                      Coord3D{w, h, static_cast<lengthType>(d > 0 ? d : 1)},
                      buf.channelsPerLight()};
    }

    /// Byte offset of a coordinate, or `bytes` when it is outside the grid: the one address
    /// computation every draw call shares, so the addressing rule (x fastest, then y, then z) has a
    /// single home.
    size_t offsetOf(Coord3D p) const {
        if (p.x < 0 || p.y < 0 || p.z < 0 || p.x >= dims.x || p.y >= dims.y || p.z >= dims.z) return bytes;
        return (static_cast<size_t>(p.z) * dims.y * dims.x
                + static_cast<size_t>(p.y) * dims.x + p.x) * cpl;
    }
};

/// One pixel, clipped to the grid: the Canvas form. Same semantics as the (Buffer&, dims) overload
/// below, which remains until the migration completes and is then removed (a permanent two-API
/// window would be worse than either shape alone).
inline void pixel(const Canvas& cv, Coord3D p, RGB c) {
    const size_t off = cv.offsetOf(p);
    if (off + (cv.cpl < 3 ? cv.cpl : 3) > cv.bytes) return;
    if (cv.cpl >= 1) cv.data[off + 0] = c.r;
    if (cv.cpl >= 2) cv.data[off + 1] = c.g;
    if (cv.cpl >= 3) cv.data[off + 2] = c.b;
}

/// Read one pixel; black when outside the grid. The Canvas form of `get`.
inline RGB get(const Canvas& cv, Coord3D p) {
    const size_t off = cv.offsetOf(p);
    if (off + (cv.cpl < 3 ? cv.cpl : 3) > cv.bytes) return RGB{0, 0, 0};
    return RGB{cv.cpl >= 1 ? cv.data[off + 0] : uint8_t{0},
               cv.cpl >= 2 ? cv.data[off + 1] : uint8_t{0},
               cv.cpl >= 3 ? cv.data[off + 2] : uint8_t{0}};
}

namespace detail {
/// Walk the pixels of a 3D Bresenham line from a to b, calling `plot` for each. Factored out so the
/// Buffer and Canvas forms of `line` share one error-carry loop instead of drifting apart.
///
/// `shorten` (0..255) pulls b back toward a by that fraction, with the *2 rounding the original
/// used: the perspective/length lever effects animate.
template <typename PlotFn>
inline void walkLine(Coord3D a, Coord3D b, uint8_t shorten, PlotFn plot) {
    if (shorten == 0) return;
    if (shorten < 255) {
        const int bx = ((2 * int(b.x) - 2 * int(a.x)) * int(shorten)) / 255 + 2 * int(a.x);
        const int by = ((2 * int(b.y) - 2 * int(a.y)) * int(shorten)) / 255 + 2 * int(a.y);
        const int bz = ((2 * int(b.z) - 2 * int(a.z)) * int(shorten)) / 255 + 2 * int(a.z);
        b = {static_cast<lengthType>((bx + 1) / 2),
             static_cast<lengthType>((by + 1) / 2),
             static_cast<lengthType>((bz + 1) / 2)};
    }
    Coord3D p = a;
    const lengthType dx = b.x > a.x ? static_cast<lengthType>(b.x - a.x) : static_cast<lengthType>(a.x - b.x);
    const lengthType dy = b.y > a.y ? static_cast<lengthType>(b.y - a.y) : static_cast<lengthType>(a.y - b.y);
    const lengthType dz = b.z > a.z ? static_cast<lengthType>(b.z - a.z) : static_cast<lengthType>(a.z - b.z);
    const lengthType sx = a.x < b.x ? 1 : -1, sy = a.y < b.y ? 1 : -1, sz = a.z < b.z ? 1 : -1;

    if (dx >= dy && dx >= dz) {
        lengthType ey = 2 * dy - dx, ez = 2 * dz - dx;
        for (lengthType i = 0; i <= dx; i++) {
            plot(p);
            if (ey >= 0) { p.y = static_cast<lengthType>(p.y + sy); ey -= 2 * dx; }
            if (ez >= 0) { p.z = static_cast<lengthType>(p.z + sz); ez -= 2 * dx; }
            ey += 2 * dy; ez += 2 * dz; p.x = static_cast<lengthType>(p.x + sx);
        }
    } else if (dy >= dx && dy >= dz) {
        lengthType ex = 2 * dx - dy, ez = 2 * dz - dy;
        for (lengthType i = 0; i <= dy; i++) {
            plot(p);
            if (ex >= 0) { p.x = static_cast<lengthType>(p.x + sx); ex -= 2 * dy; }
            if (ez >= 0) { p.z = static_cast<lengthType>(p.z + sz); ez -= 2 * dy; }
            ex += 2 * dx; ez += 2 * dz; p.y = static_cast<lengthType>(p.y + sy);
        }
    } else {
        lengthType ex = 2 * dx - dz, ey = 2 * dy - dz;
        for (lengthType i = 0; i <= dz; i++) {
            plot(p);
            if (ex >= 0) { p.x = static_cast<lengthType>(p.x + sx); ex -= 2 * dz; }
            if (ey >= 0) { p.y = static_cast<lengthType>(p.y + sy); ey -= 2 * dz; }
            ex += 2 * dx; ey += 2 * dy; p.z = static_cast<lengthType>(p.z + sz);
        }
    }
}
}  // namespace detail

// One pixel, clipped to the grid. Writes R/G/B where channels fit (cpl may be 1..N); extra
// channels (e.g. a W in RGBW) are left as-is: the driver derives white, same as effects do.
inline void pixel(Buffer& buf, Coord3D dims, Coord3D p, RGB c) {
    if (p.x < 0 || p.y < 0 || p.z < 0 || p.x >= dims.x || p.y >= dims.y || p.z >= dims.z) return;
    const uint8_t cpl = buf.channelsPerLight();
    const size_t off = (static_cast<size_t>(p.z) * dims.y * dims.x
                        + static_cast<size_t>(p.y) * dims.x + p.x) * cpl;
    if (off + (cpl < 3 ? cpl : 3) > buf.bytes()) return;   // defends a dims/buffer mismatch
    uint8_t* d = buf.data();
    if (cpl >= 1) d[off + 0] = c.r;
    if (cpl >= 2) d[off + 1] = c.g;
    if (cpl >= 3) d[off + 2] = c.b;
}

// A straight line a→b, clipped to the grid. 3D Bresenham: step along the dominant axis and carry
// an integer error term per other axis (the textbook generalisation of the 2D line). Works for
// 1D (a row), 2D (a plane), and 3D (a volume) without special-casing: a degenerate axis just
// never steps. Endpoints are inclusive.
//
// `shorten` (0..255, default 255 = full line) draws only the first shorten/255 of the way from a
// toward b: the far endpoint is pulled back toward `a`. 255 = whole line, 128 ≈ half, 1 = the
// start pixel, 0 = nothing. This is the perspective/length lever (MoonLight's `depth` param):
// effects animate the drawn tip by varying `shorten`, so a fixed pair of endpoints traces a
// sweeping partial segment over successive frames. (WLEDMM's *2-rounding shorten, generalised 3D.)
inline void line(Buffer& buf, Coord3D dims, Coord3D a, Coord3D b, RGB c, uint8_t shorten = 255) {
    detail::walkLine(a, b, shorten, [&](Coord3D p) { pixel(buf, dims, p, c); });
}

// --- Buffer read/modify helpers --------------------------------------------
// The offset of a pixel in the flat buffer, or buf.bytes() if out of bounds (caller checks).
inline size_t offsetOf(const Buffer& buf, Coord3D dims, Coord3D p) {
    if (p.x < 0 || p.y < 0 || p.z < 0 || p.x >= dims.x || p.y >= dims.y || p.z >= dims.z) return buf.bytes();
    return (static_cast<size_t>(p.z) * dims.y * dims.x + static_cast<size_t>(p.y) * dims.x + p.x)
           * buf.channelsPerLight();
}

// Read the RGB at a pixel (black if out of bounds / fewer than 3 channels).
inline RGB get(const Buffer& buf, Coord3D dims, Coord3D p) {
    const size_t off = offsetOf(buf, dims, p);
    if (off + 2 >= buf.bytes()) return {0, 0, 0};
    const uint8_t* d = buf.data();
    return {d[off + 0], d[off + 1], d[off + 2]};
}

// Blend a color into a pixel by amt/255 (amt 0 = leave as-is, 255 = replace). The in-place
// read-modify-write that GoL's dead-cell fade-to-background and age-toward-red use
// (MoonLight's blendColor). Clipped like pixel().
inline void blendPixel(Buffer& buf, Coord3D dims, Coord3D p, RGB c, uint8_t amt) {
    const size_t off = offsetOf(buf, dims, p);
    if (off + 2 >= buf.bytes()) return;
    uint8_t* d = buf.data();
    const RGB cur{d[off + 0], d[off + 1], d[off + 2]};
    const RGB out = blend(cur, c, amt);
    d[off + 0] = out.r; d[off + 1] = out.g; d[off + 2] = out.b;
}

// Add a color into a pixel, saturating (a bright pixel can't wrap to dark): WLED's addRGB / additive
// setPixelColor. Used to re-stamp a light on top of a blur so its center stays bright. Clipped like pixel().
inline void addPixel(Buffer& buf, Coord3D dims, Coord3D p, RGB c) {
    const size_t off = offsetOf(buf, dims, p);
    if (off + 2 >= buf.bytes()) return;
    uint8_t* d = buf.data();
    d[off + 0] = qadd8(d[off + 0], c.r);
    d[off + 1] = qadd8(d[off + 1], c.g);
    d[off + 2] = qadd8(d[off + 2], c.b);
}

// Fade the whole buffer toward black by amt/255: one pass over the bytes. This is the primitive the
// Layer's once-per-frame collected fade (Layer::fadeToBlackBy) applies; effects request a fade through
// the Layer (which MINs the amount across effects and calls this once) rather than calling it directly.
inline void fade(Buffer& buf, uint8_t amt) {
    const uint8_t keep = static_cast<uint8_t>(255 - amt);
    uint8_t* d = buf.data();
    const size_t n = buf.bytes();
    for (size_t i = 0; i < n; i++) d[i] = scale8(d[i], keep);
}

// Box blur, working 1D→3D against the flat Buffer: one unified primitive, not a blur1d/blur2d/blur3d
// trio (the *common patterns first* / "primitives are 3D-aware" rule, same as draw::line). It runs a
// separable seep pass along each axis whose extent is >1: a 1×N 1D layer blurs along y (its only
// axis with extent>1); 2D along x then y; 3D along x, y, z. `amt` (0 = none, 255 = max) is split
// keep=255-amt / seep=amt>>1 per pixel.
//
// Algorithm: the canonical FastLED blur1d single-forward-pass with carryover: each pixel keeps
// `keep` of itself, seeps `seep` forward to the next pixel and `seep` back to the previous one, so
// one O(N) pass per axis approximates a symmetric box blur. Behavior is identical to MoonLight's
// blur1d/blurRows/blurColumns (verified against VirtualLayer.cpp); the speed comes from doing it on
// the raw bytes: a stride walk with three uint8 carried in registers, no per-pixel getRGB/setRGB/
// Coord3D construction (the overhead that makes a generic-layer blur an FPS killer). Prior art:
// FastLED's blur1d (Mark Kriegsman), the recognisable carryover-seep; our byte-level implementation.
//
// `stride` is the byte step between adjacent pixels ALONG the blurred axis (cpl for x, w·cpl for y,
// w·h·cpl for z); `lineCount`/`lineStride` walk the starts of each parallel line. RGB only (first 3
// channels); a W channel is untouched. Saturating adds (qadd8) so a bright pixel can't wrap to dark.
inline void blurAxis(uint8_t* d, size_t cpl, size_t len, size_t stride,
                     size_t lineCount, size_t lineStride, uint8_t amt) {
    if (len < 2 || cpl < 3) return;                 // nothing to seep along a 1-pixel (or sub-RGB) axis
    const uint8_t keep = static_cast<uint8_t>(255 - amt);
    const uint8_t seep = static_cast<uint8_t>(amt >> 1);
    for (size_t l = 0; l < lineCount; l++) {
        uint8_t* base = d + l * lineStride;
        uint8_t cr = 0, cg = 0, cb = 0;             // carryover (the seep flowing forward), starts black
        size_t off = 0, prev = 0;
        for (size_t i = 0; i < len; i++, off += stride) {
            uint8_t* px = base + off;
            const uint8_t pr = scale8(px[0], seep), pg = scale8(px[1], seep), pb = scale8(px[2], seep);
            px[0] = qadd8(scale8(px[0], keep), cr);  // keep self + receive prev pixel's forward seep
            px[1] = qadd8(scale8(px[1], keep), cg);
            px[2] = qadd8(scale8(px[2], keep), cb);
            if (i) {                                 // seep back into the previous pixel (deferred add)
                uint8_t* pv = base + prev;
                pv[0] = qadd8(pv[0], pr); pv[1] = qadd8(pv[1], pg); pv[2] = qadd8(pv[2], pb);
            }
            cr = pr; cg = pg; cb = pb; prev = off;
        }
        uint8_t* last = base + prev;                 // the final forward seep lands on the last pixel
        last[0] = qadd8(last[0], cr); last[1] = qadd8(last[1], cg); last[2] = qadd8(last[2], cb);
    }
}

// Blur the whole buffer by `amt`, separably along every axis with extent >1 (x, then y, then z):
// MoonLight's blur2d order, extended to z). One call covers 1D/2D/3D. Off the per-pixel-effect path.
inline void blur(Buffer& buf, Coord3D dims, uint8_t amt) {
    if (amt == 0) return;
    uint8_t* d = buf.data();
    const size_t cpl = buf.channelsPerLight();
    const size_t w = dims.x > 0 ? static_cast<size_t>(dims.x) : 0;
    const size_t h = dims.y > 0 ? static_cast<size_t>(dims.y) : 0;
    const size_t z = dims.z > 0 ? static_cast<size_t>(dims.z) : 0;
    if (w == 0 || h == 0 || z == 0) return;
    if (static_cast<size_t>(w * h * z) * cpl > buf.bytes()) return;   // dims/buffer mismatch guard
    // x-pass: each (y,z) line is `w` pixels, stride cpl; lines start every w·cpl bytes, h·z of them.
    blurAxis(d, cpl, w, cpl, h * z, w * cpl, amt);
    // y-pass: each (x,z) line is `h` pixels, stride w·cpl. Lines: for each z, the w columns: start
    // offsets are z·(h·w·cpl) + x·cpl. Walk them as one run of (w·z) lines stepping by cpl, but the
    // z blocks aren't contiguous in column-start, so loop z outside.
    for (size_t zz = 0; zz < z; zz++)
        blurAxis(d + zz * h * w * cpl, cpl, h, w * cpl, w, cpl, amt);
    // z-pass (3D only): each (x,y) line is `z` pixels, stride w·h·cpl; w·h lines stepping by cpl.
    if (z > 1) blurAxis(d, cpl, z, w * h * cpl, w * h, cpl, amt);
}

// Fill the whole buffer with one color (MoonLight's fill_solid).
inline void fill(Buffer& buf, RGB c) {
    const uint8_t cpl = buf.channelsPerLight();
    if (cpl == 0) return;   // a 0-channel buffer has no color to write; guards off += 0 spinning
    uint8_t* d = buf.data();
    const size_t n = buf.bytes();
    for (size_t off = 0; off + cpl <= n; off += cpl) {
        if (cpl >= 1) d[off + 0] = c.r;
        if (cpl >= 2) d[off + 1] = c.g;
        if (cpl >= 3) d[off + 2] = c.b;
    }
}

// Blit one glyph of `font` at grid position (x, y). Each of the font's `height` rows is one byte;
// bit set → pixel on, columns MSB-first across the glyph's `width` (bit (7) is the left column, down
// to bit (8-width)). Only printable ASCII 32..126 is drawn; anything else is skipped (a gap). Pixels
// are clipped to the grid (draw::pixel). Prior art: the WLED/MoonLight console-font blitter shape.
inline void glyph(Buffer& buf, Coord3D dims, const fonts::Font& font, char ch, lengthType x, lengthType y, RGB c) {
    if (ch < 32 || ch > 126) return;
    const uint8_t idx = static_cast<uint8_t>(ch - 32);
    const uint8_t* rows = font.rows + static_cast<size_t>(idx) * font.height;
    for (uint8_t ry = 0; ry < font.height; ry++) {
        const uint8_t bits = rows[ry];
        // Columns are MSB-first: the LEFTMOST glyph column (rx=0) is bit 7, the next bit 6, … so
        // read column rx from bit (7 - rx). (Reading (rx + 8-width) instead mirrors each glyph
        // left-to-right: a 'b' renders as a 'd'.)
        for (uint8_t rx = 0; rx < font.width; rx++)
            if ((bits >> (7 - rx)) & 0x01)
                pixel(buf, dims, {static_cast<lengthType>(x + rx), static_cast<lengthType>(y + ry), 0}, c);
    }
}

// Draw a NUL-terminated string starting at (x, y), advancing `font.width` per character. `\n` starts
// a new line one `font.height` down and resets x to the start column (multi-line layout). Off-grid
// glyphs clip. Returns the total pixel width drawn on the first line (for scroll bookkeeping).
inline lengthType text(Buffer& buf, Coord3D dims, const fonts::Font& font, const char* str,
                       lengthType x, lengthType y, RGB c) {
    if (!str) return 0;
    lengthType cx = x, cy = y;
    lengthType firstLineWidth = 0;   // frozen at the first '\n' so a multi-line string still reports line 1
    bool onFirstLine = true;
    for (const char* p = str; *p; p++) {
        if (*p == '\n') {
            if (onFirstLine) { firstLineWidth = static_cast<lengthType>(cx - x); onFirstLine = false; }
            cx = x; cy = static_cast<lengthType>(cy + font.height); continue;
        }
        glyph(buf, dims, font, *p, cx, cy, c);
        cx = static_cast<lengthType>(cx + font.width);
    }
    return onFirstLine ? static_cast<lengthType>(cx - x) : firstLineWidth;
}

// ---- Canvas overloads --------------------------------------------------------------------------
// The Canvas forms of the primitives above. Most carry their OWN implementation rather than
// forwarding to the (Buffer&, dims) form, because `Canvas` holds a raw pointer where the older
// signatures take a Buffer&. That means a pair CAN drift, and one already did: the Canvas `blur`
// looped its y-pass differently from the Buffer form until a reviewer caught it. `line` is the
// exception: both forms share `detail::walkLine`, so its error-carry loop has exactly one home.
//
// When the last caller of the older form is gone these become the implementations and the pair
// collapses to one (§ the subtraction pass in the top-down spec), which is what removes the drift
// risk for good.

/// Fade every channel toward black. Canvas form; the Buffer form forwards here.
inline void fade(const Canvas& cv, uint8_t amt) {
    const uint8_t keep = static_cast<uint8_t>(255 - amt);
    for (size_t i = 0; i < cv.bytes; i++) cv.data[i] = scale8(cv.data[i], keep);
}

/// Decay every sample toward black by a HALF-LIFE: after `halfLifeMs`, half of it is gone.
///
/// The framerate-independent form of `fade`, and the one to reach for on state that persists across
/// frames (a trail plane, an advected field). `fade` takes "how much to lose this frame", which
/// means the same setting is a long tail at 60 fps and an instant clear at 1200; this takes a
/// duration, so the picture is identical on any device and `dt` does the work.
///
/// The weight is computed ONCE per call (`mm::halfLifeKeep`) and the loop is a multiply and a shift
/// per byte, so this costs what `fade` costs.
///
/// **The 8-bit limit, measured (2026-09-04).** An 8-BIT buffer cannot hold this decay at a high
/// framerate, and no rounding rule fixes it. Decaying 200 over a 500 ms half-life in 500 ms of
/// frames, where the exact answer is 100:
///
/// | frame | truncating | rounding | a 16-bit accumulator |
/// |---|---|---|---|
/// | 50 ms | 96 | 100 | 100 |
/// | 5 ms | 73 | 100 | 101 |
/// | 1 ms | **0** | **200** | 102 |
///
/// Truncating loses a fraction every frame until a fast device erases the trail outright; rounding
/// puts it back every frame until the trail never decays and the effect turns solid, which is the
/// exact symptom `fade` already has. Both failures are the QUANTIZATION, not the weight: the value
/// is re-rounded to a byte hundreds of times a second. So an effect whose trail must survive at any
/// framerate keeps its plane WIDER than the layer (16 bits per channel in its own ScratchBuffer)
/// and narrows once on the way out, which is where the precision belongs. `decay` on a byte plane
/// is honest for a slow cadence (a 50 ms tick, or a half-life short enough that a frame's loss is
/// several counts) and is what the Layer's collected `fadeToBlackBy` already does.
inline void decay(const Canvas& cv, uint32_t halfLifeMs, uint32_t dtMs) {
    const uint32_t keep = mm::halfLifeKeep(dtMs, halfLifeMs);
    if (keep >= 65536) return;                 // nothing elapsed, or no half-life asked for
    if (keep == 0) { std::memset(cv.data, 0, cv.bytes); return; }
    for (size_t i = 0; i < cv.bytes; i++)
        cv.data[i] = static_cast<uint8_t>((static_cast<uint32_t>(cv.data[i]) * keep) >> 16);
}

/// The same decay over a 16-bit plane: the form a trail uses, and the one that holds at any
/// framerate (the table above). `n` is the number of 16-bit samples, not bytes.
///
/// Separate from the Canvas form rather than a template over it, because the two are different
/// enough to be worth reading apart: this one owns no geometry, since a scratch plane is a flat
/// array its effect already knows the shape of.
inline void decay16(uint16_t* data, size_t n, uint32_t halfLifeMs, uint32_t dtMs) {
    if (!data) return;
    const uint32_t keep = mm::halfLifeKeep(dtMs, halfLifeMs);
    if (keep >= 65536) return;
    if (keep == 0) { std::memset(data, 0, n * sizeof(uint16_t)); return; }
    for (size_t i = 0; i < n; i++)
        data[i] = static_cast<uint16_t>((static_cast<uint32_t>(data[i]) * keep) >> 16);
}

/// Fill every light with one color, leaving any channel beyond RGB untouched.
inline void fill(const Canvas& cv, RGB c) {
    if (cv.cpl == 0) return;   // a 0-channel buffer has no color to write
    for (size_t off = 0; off + cv.cpl <= cv.bytes; off += cv.cpl) {
        if (cv.cpl >= 1) cv.data[off + 0] = c.r;
        if (cv.cpl >= 2) cv.data[off + 1] = c.g;
        if (cv.cpl >= 3) cv.data[off + 2] = c.b;
    }
}

/// Read-modify-write lerp of one pixel toward `c` by `amt`.
inline void blendPixel(const Canvas& cv, Coord3D p, RGB c, uint8_t amt) {
    const RGB cur = get(cv, p);
    pixel(cv, p, blend(cur, c, amt));
}

/// Saturating additive pixel: light adds, so this never wraps to black.
inline void addPixel(const Canvas& cv, Coord3D p, RGB c) {
    const RGB cur = get(cv, p);
    pixel(cv, p, RGB{qadd8(cur.r, c.r), qadd8(cur.g, c.g), qadd8(cur.b, c.b)});
}

/// Separable box blur over every axis with extent > 1: one call covers 1D/2D/3D. The axis passes
/// are the Buffer form's: the y-pass loops z on the OUTSIDE because a z-slice's column starts are
/// not contiguous, which a single call cannot express.
inline void blur(const Canvas& cv, uint8_t amt) {
    if (amt == 0 || cv.cpl == 0) return;
    const size_t cpl = cv.cpl;
    const size_t w = cv.dims.x > 0 ? static_cast<size_t>(cv.dims.x) : 0;
    const size_t h = cv.dims.y > 0 ? static_cast<size_t>(cv.dims.y) : 0;
    const size_t z = cv.dims.z > 0 ? static_cast<size_t>(cv.dims.z) : 0;
    if (w == 0 || h == 0 || z == 0) return;
    if (w * h * z * cpl > cv.bytes) return;                 // dims/buffer mismatch guard
    blurAxis(cv.data, cpl, w, cpl, h * z, w * cpl, amt);    // x
    for (size_t zz = 0; zz < z; zz++)                       // y, per z-slice
        blurAxis(cv.data + zz * h * w * cpl, cpl, h, w * cpl, w, cpl, amt);
    if (z > 1) blurAxis(cv.data, cpl, z, w * h * cpl, w * h, cpl, amt);   // z
}

/// A straight line a→b on a Canvas: the same 3D Bresenham the Buffer form runs, reached through
/// the shared walker below so the error-carry loop has exactly one home.
inline void line(const Canvas& cv, Coord3D a, Coord3D b, RGB c, uint8_t shorten = 255) {
    detail::walkLine(a, b, shorten, [&](Coord3D p) { pixel(cv, p, c); });
}

// --- Blob field (metaballs) -------------------------------------------------------------------
//
// A field of N sources orbiting on sine paths, summed per pixel with an inverse-square falloff.
// This is the standard implicit-surface primitive: anything that reads as fluid, merging, molten or
// organic is this field under a different coloring: blobs, plasma cores, glowing orbs, an audio
// band driving a source's radius. Two effects use it today (they computed byte-identical fields
// from two private copies of the loop) and differ ONLY in the coloring they apply, which is the
// evidence for the seam: the field is shared, the coloring is the effect's own.
//
// The oscillator tables stay with the CALLER, so a caller is free to drive sources from anything:
// a control, audio, a particle position: rather than from the sine paths the current callers use.
// A kernel that hard-coded one set of constants would have made every future effect look the same.
//
// Prior art: Jim Blinn's 1982 blobby model; the integer inverse-square form follows WLED's metaball
// effects rather than Blinn's exponential.

/// One blob's orbit: where it sits at time `t` on a grid of `w` by `h`.
struct BlobPath {
    uint8_t speedMul;   ///< multiplies the shared time base, so blobs drift apart
    uint8_t phaseX;     ///< phase offset on x, keeping the paths from coinciding
    uint8_t phaseY;
};

/// Evaluate blob centers for this frame into `outX`/`outY` (caller-sized to `count`).
inline void blobCenters(const BlobPath* paths, uint8_t count, uint8_t t, lengthType w, lengthType h,
                        int16_t* outX, int16_t* outY) {
    for (uint8_t b = 0; b < count; b++) {
        const uint8_t tb = static_cast<uint8_t>(t * paths[b].speedMul);
        outX[b] = static_cast<int16_t>((sin8(static_cast<uint8_t>(tb + paths[b].phaseX)) * w) >> 8);
        outY[b] = static_cast<int16_t>((sin8(static_cast<uint8_t>(tb + paths[b].phaseY)) * h) >> 8);
    }
}

/// `r2` is a squared radius in WHOLE PIXELS, not sub-pixel units: `r2 * 64` below must stay inside
/// int32, which holds for any radius up to ~5700 px and so for any uint8-controlled radius, but a
/// caller passing a 24.8 sub-pixel radius squared would overflow.
///
/// The field at one pixel: the sum over blobs of r²·64 / (d² + 1). The `+1` keeps a pixel sitting
/// exactly on a center from dividing by zero, and the ·64 holds precision in the integer divide.
/// Returns the raw sum; the caller decides how it maps to color, and how it clamps.
inline uint32_t blobField(lengthType x, lengthType y, const int16_t* bx, const int16_t* by,
                          uint8_t count, int32_t r2) {
    uint32_t field = 0;
    for (uint8_t b = 0; b < count; b++) {
        const int32_t dx = static_cast<int32_t>(x) - bx[b];
        const int32_t dy = static_cast<int32_t>(y) - by[b];
        const int32_t d2 = dx * dx + dy * dy + 1;
        field += static_cast<uint32_t>((r2 * 64) / d2);
    }
    return field;
}

// --- Scrolling ------------------------------------------------------------------------------
//
// A shift register over the whole grid: every light moves `delta` steps along one axis, and the
// vacated edge is left dark (the caller paints the new content into it). FreqMatrix hand-rolled this
// as a per-pixel copy loop reading each neighbor through get()/pixel(); the same move is one
// `memmove` per contiguous run, because the addressing rule puts x adjacent in memory and a whole
// row adjacent along y.
//
// `wrap` chooses between the two useful behaviors: a shift register (false: content falls off the
// end and the far edge goes dark) and a loop (true: content that leaves one edge re-enters the
// other), which is the marquee/tunnel idiom. Prior art: WLED's `move()` and the classic shift
// register; the wrapping form is the standard scrolling-texture primitive.

/// Move the grid `delta` steps along `axis` (0 = x, 1 = y, 2 = z). Positive delta moves toward
/// increasing coordinates. Vacated cells go dark unless `wrap` is set.
inline void scroll(const Canvas& cv, uint8_t axis, int delta, bool wrap = false) {
    if (delta == 0 || cv.data == nullptr) return;
    const lengthType extent = axis == 0 ? cv.dims.x : (axis == 1 ? cv.dims.y : cv.dims.z);
    if (extent <= 1) return;                       // nothing to move along a degenerate axis

    // A non-wrapping shift of a whole extent moves everything off the grid, so it is a clear. (Check
    // this BEFORE reducing modulo the extent, which would turn it into a no-op.)
    if (!wrap && (delta >= extent || delta <= -extent)) { fill(cv, RGB{0, 0, 0}); return; }

    // Wrapping reduces into the axis and normalizes to a positive rotation, since rotating left by n
    // equals rotating right by extent-n. A shift keeps its sign: the direction decides which end
    // goes dark, so it cannot be normalized away.
    int shift = delta % extent;
    if (shift == 0) return;                        // a full turn when wrapping; nothing to do
    if (wrap && shift < 0) shift += extent;

    // Every axis is a sequence of equally-spaced "lines" of `extent` cells with a fixed step, so one
    // loop covers all three once the geometry is named.
    const size_t cpl = cv.cpl;
    const size_t rowBytes = static_cast<size_t>(cv.dims.x) * cpl;
    const size_t sliceBytes = static_cast<size_t>(cv.dims.y) * rowBytes;
    //
    // A line's base is `outer * outerStride + inner * innerStride`, two nested counts rather than
    // one: along Y the lines are the (z, x) columns, and those are NOT evenly spaced by a single
    // stride (x steps by cpl within a slice, z steps by a whole slice). Collapsing them to one
    // counter scrolled the first column of each slice and left every other column standing.
    size_t step = 0, outer = 0, outerStride = 0, inner = 1, innerStride = 0;
    switch (axis) {
        case 0:  step = cpl;        outer = static_cast<size_t>(cv.dims.y) * cv.dims.z; outerStride = rowBytes;
                 inner = 1;         innerStride = 0;                                                            break;
        case 1:  step = rowBytes;   outer = cv.dims.z;                                  outerStride = sliceBytes;
                 inner = cv.dims.x; innerStride = cpl;                                                          break;
        default: step = sliceBytes; outer = cv.dims.y;                                  outerStride = rowBytes;
                 inner = cv.dims.x; innerStride = cpl;                                                          break;
    }
    const size_t lineCount = outer * inner;

    // One cell of scratch is enough for the wrapping case if we rotate in place, but a rotation by
    // an arbitrary amount needs somewhere to hold the part that wraps around. The largest run we
    // ever hold is one line, and a line is at most the grid's longest axis.
    for (size_t line = 0; line < lineCount; line++) {
        uint8_t* base = cv.data + (line / inner) * outerStride + (line % inner) * innerStride;
        // Skip a line that would read past the buffer rather than abandoning the whole scroll: a
        // dims/buffer mismatch should cost that line, not leave every later line unscrolled.
        if (base + static_cast<size_t>(extent - 1) * step + cpl > cv.data + cv.bytes) continue;

        if (step == cpl) {
            // Contiguous run: the whole line is one memmove.
            uint8_t* p = base;
            const size_t n = static_cast<size_t>(extent) * cpl;
            const size_t off = static_cast<size_t>(shift > 0 ? shift : 0) * cpl;
            if (wrap) {
                // Rotate right by `off` bytes: reverse the two parts, then the whole (the standard
                // in-place rotation, no scratch buffer).
                std::reverse(p, p + n - off);
                std::reverse(p + n - off, p + n);
                std::reverse(p, p + n);
            } else if (shift > 0) {
                std::memmove(p + off, p, n - off);   // toward increasing x: the low end goes dark
                std::memset(p, 0, off);
            } else {
                const size_t back = static_cast<size_t>(-shift) * cpl;
                std::memmove(p, p + back, n - back); // toward decreasing x: the high end goes dark
                std::memset(p + n - back, 0, back);
            }
        } else {
            // Strided run (a column, or a z-line): move cell by cell, far end first so a forward
            // shift does not overwrite a source it has yet to read.
            if (wrap) {
                // Rotate by three reversals, the same trick the contiguous path above uses. No
                // scratch buffer, so a light of ANY channel count rotates whole: a fixed-size
                // temporary silently truncated the extra channels of a wide fixture, and there is
                // no ceiling worth guessing at here. It is also O(extent) instead of
                // O(shift x extent): a 64-row column scrolled by 30 was doing 1920 cell copies.
                const auto swapCells = [&](int i, int j) {
                    uint8_t* a = base + static_cast<size_t>(i) * step;
                    uint8_t* b = base + static_cast<size_t>(j) * step;
                    for (size_t k = 0; k < cpl; k++) { const uint8_t tmp = a[k]; a[k] = b[k]; b[k] = tmp; }
                };
                const auto reverseRange = [&](int lo, int hi) {
                    while (lo < hi) { swapCells(lo, hi); lo++; hi--; }
                };
                reverseRange(0, extent - 1 - shift);
                reverseRange(extent - shift, extent - 1);
                reverseRange(0, extent - 1);
            } else if (shift > 0) {
                // Far end first, so a source cell is read before the shift overwrites it.
                for (int c = extent - 1; c >= 0; c--) {
                    const int src = c - shift;
                    uint8_t* dst = base + static_cast<size_t>(c) * step;
                    if (src >= 0) std::memcpy(dst, base + static_cast<size_t>(src) * step, cpl);
                    else          std::memset(dst, 0, cpl);
                }
            } else {
                // Negative shift walks the other way, near end first, for the same reason.
                for (int c = 0; c < extent; c++) {
                    const int src = c - shift;                  // shift < 0, so src > c
                    uint8_t* dst = base + static_cast<size_t>(c) * step;
                    if (src < extent) std::memcpy(dst, base + static_cast<size_t>(src) * step, cpl);
                    else              std::memset(dst, 0, cpl);
                }
            }
        }
    }
}

// --- Rectangles and bars -----------------------------------------------------------------------
//
// `bar` is the audio-meter staple: a run of `len` cells growing from an origin along one axis. Four
// effects hand-rolled it, and their loops disagreed on everything that matters: which end is the
// floor, whether the color varies along the run, and what happens when the run overshoots the grid.
//
// The color is a CALLBACK rather than a single RGB because that is what the call sites actually do:
// GEQ colors by row height OR by column, AudioSpectrum's VU bar ramps green→red along its length,
// and its spectrum bars color per band. A plain `bar(..., RGB)` would have left every one of them
// with its loop, so the primitive would have earned nothing. The callback takes the cell's index
// ALONG the bar (0 = at the origin), which is the number each of those formulas was already
// computing. `RGB` overloads below cover the flat case without making callers write a lambda.
//
// **What a MoonLive script gets, stated because the two forms are NOT equivalent.** A script calls
// builtins through a table that carries plain scalars (`core/moonlive/MoonLiveBuiltins.h`), and the
// grammar is a function-call statement with expression arguments: no loops, no closures. So a
// script can reach the flat form, `bar(x, y, len, dir, r, g, b)`, and gets a SOLID bar. The
// per-cell gradient GEQ draws is not expressible that way, and a color-callback builtin cannot be
// registered in that table at all. Closing that gap needs a scalar-shaped primitive a script can
// call: a gradient bar taking two endpoint colors, or a palette-ramp variant: which is a
// deliberate addition, not something the callback form provides for free. Tracked in the
// power-function plan rather than assumed away here.

/// Direction a bar grows from its origin. Named rather than a signed delta: the call sites read as
/// "up from the floor", and a `-1` would leave the reader deriving which axis it applies to.
enum class Grow : uint8_t { Right, Left, Up, Down };

/// A run of `len` cells from `(x, y)` along `dir`, colored per cell by `colorAt(i)` where `i` is the
/// distance from the origin. Clipped per cell, so a bar longer than the grid simply stops.
template <typename ColorFn>
inline void bar(const Canvas& cv, lengthType x, lengthType y, lengthType len, Grow dir,
                ColorFn colorAt) {
    for (lengthType i = 0; i < len; i++) {
        lengthType px = x, py = y;
        switch (dir) {
            case Grow::Right: px = static_cast<lengthType>(x + i); break;
            case Grow::Left:  px = static_cast<lengthType>(x - i); break;
            case Grow::Up:    py = static_cast<lengthType>(y - i); break;   // row 0 is the top
            case Grow::Down:  py = static_cast<lengthType>(y + i); break;
        }
        pixel(cv, {px, py, 0}, colorAt(i));
    }
}

/// A bar in one flat color.
inline void bar(const Canvas& cv, lengthType x, lengthType y, lengthType len, Grow dir, RGB c) {
    bar(cv, x, y, len, dir, [c](lengthType) { return c; });
}

/// Filled axis-aligned rectangle from `(x, y)`, `w` by `h`, colored per row by `colorAt(row)`.
/// Negative or zero extents draw nothing; the edges clip.
template <typename ColorFn>
inline void fillRect(const Canvas& cv, lengthType x, lengthType y, lengthType w, lengthType h,
                     ColorFn colorAt) {
    for (lengthType row = 0; row < h; row++)
        bar(cv, x, static_cast<lengthType>(y + row), w, Grow::Right,
            [&](lengthType) { return colorAt(row); });
}

/// A filled rectangle in one flat color.
inline void fillRect(const Canvas& cv, lengthType x, lengthType y, lengthType w, lengthType h, RGB c) {
    fillRect(cv, x, y, w, h, [c](lengthType) { return c; });
}

/// The outline of an axis-aligned rectangle: four bars, corners written once each.
inline void rect(const Canvas& cv, lengthType x, lengthType y, lengthType w, lengthType h, RGB c) {
    if (w <= 0 || h <= 0) return;
    const lengthType right = static_cast<lengthType>(x + w - 1);
    const lengthType bottom = static_cast<lengthType>(y + h - 1);
    bar(cv, x, y, w, Grow::Right, c);                                    // top
    if (h == 1) return;                                                  // a 1-row rect is one bar
    bar(cv, x, bottom, w, Grow::Right, c);                               // bottom
    if (h <= 2) return;                                                  // no interior rows to join
    const lengthType inner = static_cast<lengthType>(h - 2);
    bar(cv, x, static_cast<lengthType>(y + 1), inner, Grow::Down, c);    // left
    if (w > 1) bar(cv, right, static_cast<lengthType>(y + 1), inner, Grow::Down, c);
}

// --- Circles ---------------------------------------------------------------------------------
//
// Two implementations, deliberately, because they answer different questions. `circle`/`fillCircle`
// take INTEGER pixel coordinates and use Bresenham's midpoint algorithm: no multiply per pixel, no
// distance, exactly the cells on the rim. That is the right tool when a shape sits on the grid.
//
// The SDF forms above (`sdCircle` + `coverage`) are the right tool when the circle MOVES: they give
// a sub-pixel position and an anti-aliased edge for the cost of a distance per pixel. Neither
// subsumes the other, so both stay, and the choice is named here rather than left to the reader.
//
// Prior art: Bresenham's 1965 midpoint circle, the textbook form.

/// The outline of a circle, integer center and radius, using the midpoint algorithm. Each of the
/// eight octants is mirrored from one computed arc, so the rim is exact and symmetric.
inline void circle(const Canvas& cv, lengthType cx, lengthType cy, lengthType r, RGB c) {
    if (r < 0) return;
    if (r == 0) { pixel(cv, {cx, cy, 0}, c); return; }
    lengthType x = 0, y = r;
    int d = 3 - 2 * r;                     // the midpoint decision variable
    while (x <= y) {
        // Eight-way symmetry: one computed point paints eight rim cells.
        pixel(cv, {static_cast<lengthType>(cx + x), static_cast<lengthType>(cy + y), 0}, c);
        pixel(cv, {static_cast<lengthType>(cx - x), static_cast<lengthType>(cy + y), 0}, c);
        pixel(cv, {static_cast<lengthType>(cx + x), static_cast<lengthType>(cy - y), 0}, c);
        pixel(cv, {static_cast<lengthType>(cx - x), static_cast<lengthType>(cy - y), 0}, c);
        pixel(cv, {static_cast<lengthType>(cx + y), static_cast<lengthType>(cy + x), 0}, c);
        pixel(cv, {static_cast<lengthType>(cx - y), static_cast<lengthType>(cy + x), 0}, c);
        pixel(cv, {static_cast<lengthType>(cx + y), static_cast<lengthType>(cy - x), 0}, c);
        pixel(cv, {static_cast<lengthType>(cx - y), static_cast<lengthType>(cy - x), 0}, c);
        if (d < 0) { d += 4 * x + 6; }
        else       { d += 4 * (x - y) + 10; y--; }
        x++;
    }
}

/// A filled disc: the same midpoint walk, drawing a horizontal span per scanline instead of points,
/// colored per row by `colorAt(dyFromCenter)`: the signed row offset, so a caller can ramp a
/// gradient across the disc.
template <typename ColorFn>
inline void fillCircle(const Canvas& cv, lengthType cx, lengthType cy, lengthType r, ColorFn colorAt) {
    if (r < 0) return;
    lengthType x = 0, y = r;
    int d = 3 - 2 * r;
    while (x <= y) {
        // Two span pairs per step: the arc is walked in x, and each point fixes the width of a row.
        bar(cv, static_cast<lengthType>(cx - x), static_cast<lengthType>(cy + y),
            static_cast<lengthType>(2 * x + 1), Grow::Right, colorAt(y));
        bar(cv, static_cast<lengthType>(cx - x), static_cast<lengthType>(cy - y),
            static_cast<lengthType>(2 * x + 1), Grow::Right, colorAt(static_cast<lengthType>(-y)));
        bar(cv, static_cast<lengthType>(cx - y), static_cast<lengthType>(cy + x),
            static_cast<lengthType>(2 * y + 1), Grow::Right, colorAt(x));
        bar(cv, static_cast<lengthType>(cx - y), static_cast<lengthType>(cy - x),
            static_cast<lengthType>(2 * y + 1), Grow::Right, colorAt(static_cast<lengthType>(-x)));
        if (d < 0) { d += 4 * x + 6; }
        else       { d += 4 * (x - y) + 10; y--; }
        x++;
    }
}

/// A filled disc in one flat color.
inline void fillCircle(const Canvas& cv, lengthType cx, lengthType cy, lengthType r, RGB c) {
    fillCircle(cv, cx, cy, r, [c](lengthType) { return c; });
}

// --- Anti-aliased line ------------------------------------------------------------------------
//
// Wu's 1991 algorithm: where Bresenham picks ONE cell per step, Wu lights the two cells straddling
// the true line and splits the intensity between them by distance. The result is a line without
// staircase edges, at roughly twice the writes. `line` (Bresenham, above) stays the default: this
// is for the cases where the stair-stepping is the thing you notice.
//
// The blend is additive so a line crossing existing content brightens rather than replaces it,
// matching `splat`, the other sub-pixel writer.

/// A 2D anti-aliased line between two integer endpoints (Wu 1991), z taken from `a`.
inline void lineAA(const Canvas& cv, Coord3D a, Coord3D b, RGB c) {
    int x0 = a.x, y0 = a.y, x1 = b.x, y1 = b.y;
    const bool steep = (y1 > y0 ? y1 - y0 : y0 - y1) > (x1 > x0 ? x1 - x0 : x0 - x1);
    if (steep) { std::swap(x0, y0); std::swap(x1, y1); }       // walk the major axis
    if (x0 > x1) { std::swap(x0, x1); std::swap(y0, y1); }

    const int dx = x1 - x0;
    const int dy = y1 - y0;
    // A zero-length line is a point; without this the gradient divides by zero.
    if (dx == 0) { pixel(cv, a, c); return; }

    // Fixed-point gradient in 16.16, so the walk stays integer.
    const int32_t gradient = (static_cast<int32_t>(dy) << 16) / dx;
    int32_t inter = (static_cast<int32_t>(y0) << 16);

    for (int x = x0; x <= x1; x++) {
        const int32_t whole = inter >> 16;
        const uint8_t frac = static_cast<uint8_t>((inter >> 8) & 0xFF);
        const uint8_t weightNear = static_cast<uint8_t>(255 - frac);   // `near` is a Windows macro
        // The two cells straddling the true line, weighted by how close it passes to each.
        if (steep) {
            addPixel(cv, {static_cast<lengthType>(whole), static_cast<lengthType>(x), a.z},
                     RGB{scale8(c.r, weightNear), scale8(c.g, weightNear), scale8(c.b, weightNear)});
            addPixel(cv, {static_cast<lengthType>(whole + 1), static_cast<lengthType>(x), a.z},
                     RGB{scale8(c.r, frac), scale8(c.g, frac), scale8(c.b, frac)});
        } else {
            addPixel(cv, {static_cast<lengthType>(x), static_cast<lengthType>(whole), a.z},
                     RGB{scale8(c.r, weightNear), scale8(c.g, weightNear), scale8(c.b, weightNear)});
            addPixel(cv, {static_cast<lengthType>(x), static_cast<lengthType>(whole + 1), a.z},
                     RGB{scale8(c.r, frac), scale8(c.g, frac), scale8(c.b, frac)});
        }
        inter += gradient;
    }
}

namespace sprites {
/// A small movable bitmap: palette-indexed pixels (index 0 = the transparent key, the classic
/// key-color scheme), frames stacked vertically (frame f = rows [f*h, (f+1)*h)). Carried as
/// constexpr data, the fonts.h shape. Full alpha (Porter-Duff over) stays deferred until a
/// consumer needs it; a transparent index is what classic sprites used and what LED walls need.
struct Sprite {
    const uint8_t* pixels;    // w * h * frames bytes, palette-indexed
    const RGB* palette;       // the sprite's own colors; index 0 is never read
    uint8_t w, h, frames, paletteCount;
};
}  // namespace sprites

/// Blit one sprite frame at pixel (x, y): the multi-color sibling of `glyph`. Index 0 skips
/// (transparent), an out-of-range palette index renders nothing (visible degrade, never UB),
/// `frame` clamps to the last one. `scale` is nearest-neighbor integer magnification (each
/// sprite pixel becomes a scale x scale block), which keeps pixel art crisp on a big grid.
/// `flipX` mirrors the sprite horizontally, so art drawn facing one way serves both directions
/// without a second copy of every frame.
/// Every write goes through draw::pixel, so clipping at all four edges is offsetOf's sentinel,
/// exactly as glyph clips.
inline void sprite(const Canvas& cv, const sprites::Sprite& s, uint8_t frame,
                   lengthType x, lengthType y, uint8_t scale = 1, bool flipX = false) {
    if (!s.pixels || !s.palette || s.w == 0 || s.h == 0 || s.frames == 0 || scale == 0) return;
    if (frame >= s.frames) frame = static_cast<uint8_t>(s.frames - 1);
    const uint8_t* rows = s.pixels + static_cast<size_t>(frame) * s.w * s.h;
    for (uint8_t ry = 0; ry < s.h; ry++) {
        for (uint8_t rx = 0; rx < s.w; rx++) {
            // flipX mirrors the READ, not the write, so the sprite still lands at (x, y) with the
            // same footprint. Art that faces one way (a fish, a car, a walking figure) otherwise
            // needs a second copy of every frame purely to face the other, which doubles the art
            // and its maintenance for a transform this costs one subtraction.
            const uint8_t sx0 = flipX ? static_cast<uint8_t>(s.w - 1 - rx) : rx;
            const uint8_t idx = rows[static_cast<size_t>(ry) * s.w + sx0];
            if (idx == 0 || idx >= s.paletteCount) continue;
            const RGB c = s.palette[idx];
            for (uint8_t sy = 0; sy < scale; sy++)
                for (uint8_t sx = 0; sx < scale; sx++)
                    pixel(cv, {static_cast<lengthType>(x + rx * scale + sx),
                               static_cast<lengthType>(y + ry * scale + sy), 0}, c);
        }
    }
}

/// Blit one glyph on a Canvas: the Canvas form of `glyph`. Same MSB-first column order and the
/// same clipping; only the pixel writer differs.
inline void glyph(const Canvas& cv, const fonts::Font& font, char ch, lengthType x, lengthType y, RGB c) {
    if (ch < 32 || ch > 126) return;
    const uint8_t idx = static_cast<uint8_t>(ch - 32);
    const uint8_t* rows = font.rows + static_cast<size_t>(idx) * font.height;
    for (uint8_t ry = 0; ry < font.height; ry++) {
        const uint8_t bits = rows[ry];
        for (uint8_t rx = 0; rx < font.width; rx++)
            if ((bits >> (7 - rx)) & 0x01)
                pixel(cv, {static_cast<lengthType>(x + rx), static_cast<lengthType>(y + ry), 0}, c);
    }
}

/// Draw a NUL-terminated string on a Canvas: the Canvas form of `text`. Returns the first line's
/// pixel width, as the Buffer form does.
inline lengthType text(const Canvas& cv, const fonts::Font& font, const char* str,
                       lengthType x, lengthType y, RGB c) {
    if (!str) return 0;
    lengthType cx = x, cy = y;
    lengthType firstLineWidth = 0;
    bool onFirstLine = true;
    for (const char* p = str; *p; p++) {
        if (*p == '\n') {
            if (onFirstLine) { firstLineWidth = static_cast<lengthType>(cx - x); onFirstLine = false; }
            cx = x; cy = static_cast<lengthType>(cy + font.height); continue;
        }
        glyph(cv, font, *p, cx, cy, c);
        cx = static_cast<lengthType>(cx + font.width);
    }
    return onFirstLine ? static_cast<lengthType>(cx - x) : firstLineWidth;
}

/// Byte offset of a coordinate on a Canvas, or `bytes` when it is outside the grid. The Canvas form
/// of `offsetOf`: for effects that need the raw index (e.g. to touch a W channel `pixel` leaves
/// alone). Delegates to Canvas::offsetOf so the addressing rule keeps one home.
inline size_t offsetOf(const Canvas& cv, Coord3D p) { return cv.offsetOf(p); }

// ---- Sub-pixel positioning ----------------------------------------------------------------------

/// A position in **24.8 fixed point**: one pixel = 256 sub-units, so `x >> 8` is the pixel and the
/// low byte is the fraction within it. int32 covers ±8 million pixels: a 16K-light strip has room
/// to spare, where an int16 sub-pixel type (WLED-PS uses 10.6) runs out at ±512.
using pos_t = int32_t;

/// One pixel = this many sub-units. `pixels << kSubShift` converts, `sub >> kSubShift` decodes.
inline constexpr uint8_t kSubShift = 8;
inline constexpr int32_t kSubOne = 1 << kSubShift;

/// Convert a whole-pixel coordinate to sub-pixel space.
inline constexpr pos_t toSub(lengthType px) { return static_cast<pos_t>(px) << kSubShift; }

/// Decode a sub-pixel coordinate to the pixel that contains it. Uses an arithmetic shift, which
/// floors toward negative infinity: the behavior a grid wants, so -0.5 lands in pixel -1 rather
/// than being pulled to 0 and doubling up on the boundary.
inline constexpr lengthType toPixel(pos_t sub) { return static_cast<lengthType>(sub >> kSubShift); }

/// Draw a point at a FRACTIONAL position, spreading its light across the neighboring pixels by
/// how much of each it covers (Xiaolin Wu, SIGGRAPH 1991; the same weighting WLED's `wu_pixel` and
/// its particle renderer use).
///
/// Why it matters: a whole-pixel write makes a moving point jump from cell to cell, which on a
/// coarse matrix reads as stepping. Splitting the light between neighbors by coverage lets the eye
/// see it *between* pixels, so a 16x16 panel gains apparent resolution. This is the difference the
/// canon survey ranks as the single highest-leverage primitive for motion.
///
/// Additive with saturation, because light adds: two points landing on one pixel brighten it rather
/// than one overwriting the other. Weights are computed once per axis and are exact: the four
/// corner weights sum to 256, so a point contributes exactly its own brightness, no more.
///
/// 1D/2D/3D: a degenerate axis (extent 1) contributes no second neighbor, so the same call is a
/// 2-pixel blend on a strand, 4 on a matrix, 8 in a volume.
inline void splat(const Canvas& cv, pos_t x, pos_t y, pos_t z, RGB c) {
    const lengthType px = toPixel(x), py = toPixel(y), pz = toPixel(z);
    // Fraction within the pixel, 0..255. Masking (rather than subtracting) is correct for negatives
    // too: -1.25 px has fraction 0.75 relative to pixel -2, which is what the neighbor weighting
    // needs.
    const uint16_t fx = static_cast<uint16_t>(x & (kSubOne - 1));
    const uint16_t fy = static_cast<uint16_t>(y & (kSubOne - 1));
    const uint16_t fz = static_cast<uint16_t>(z & (kSubOne - 1));

    for (uint8_t corner = 0; corner < 8; corner++) {
        const bool dx = corner & 1, dy = corner & 2, dz = corner & 4;
        // A corner on a degenerate axis is the same pixel as its partner; skip it so its light is
        // not counted twice.
        if (dx && cv.dims.x <= 1 && fx == 0) continue;
        if (dy && cv.dims.y <= 1 && fy == 0) continue;
        if (dz && cv.dims.z <= 1 && fz == 0) continue;

        const uint32_t wx = dx ? fx : (kSubOne - fx);
        const uint32_t wy = dy ? fy : (kSubOne - fy);
        const uint32_t wz = dz ? fz : (kSubOne - fz);
        // Weight is the covered fraction of this corner: the product of the per-axis coverages,
        // normalized back to 0..255 (>>16 for the two extra 8-bit factors).
        const uint32_t w = (wx * wy * wz) >> (2 * kSubShift);
        if (w == 0) continue;

        const Coord3D p{static_cast<lengthType>(px + (dx ? 1 : 0)),
                        static_cast<lengthType>(py + (dy ? 1 : 0)),
                        static_cast<lengthType>(pz + (dz ? 1 : 0))};
        addPixel(cv, p, RGB{static_cast<uint8_t>((c.r * w) >> kSubShift),
                            static_cast<uint8_t>((c.g * w) >> kSubShift),
                            static_cast<uint8_t>((c.b * w) >> kSubShift)});
    }
}

/// 2D convenience: the z axis sits at pixel 0.
inline void splat(const Canvas& cv, pos_t x, pos_t y, RGB c) { splat(cv, x, y, 0, c); }

// --- Gather (reading the grid as a texture) ----------------------------------------------------
//
// Everything above WRITES. These read, which is the other half of the vocabulary and the one the
// canon survey found missing: once a grid can be sampled at an arbitrary sub-pixel coordinate, a
// whole family follows from three lines each: feedback, tunnels, zoom, rotation, plasma warp,
// motion trails. Without it, each of those needs its own bespoke loop.
//
// `sampleWrap` takes SUB-PIXEL coordinates (the same 24.8 `pos_t` as splat) and returns a bilinear
// blend of the four surrounding pixels, wrapping at the edges. Wrapping rather than clamping is
// what makes a tunnel or a scroll seamless; clamping smears the edge pixel instead.

/// Bilinear sample at a sub-pixel coordinate, wrapping at the grid edges.
inline RGB sampleWrap(const Canvas& cv, pos_t x, pos_t y, lengthType z = 0) {
    if (cv.dims.x <= 0 || cv.dims.y <= 0) return RGB{0, 0, 0};
    // Floor to the containing pixel and keep the fraction for the blend.
    const int32_t x0 = toPixel(x), y0 = toPixel(y);
    const uint8_t fx = static_cast<uint8_t>(x & (kSubOne - 1));
    const uint8_t fy = static_cast<uint8_t>(y & (kSubOne - 1));
    // Positive modulo: a negative coordinate wraps to the far edge rather than off the grid.
    const auto wrap = [](int32_t v, lengthType n) {
        const int32_t m = v % n;
        return static_cast<lengthType>(m < 0 ? m + n : m);
    };
    const lengthType xa = wrap(x0, cv.dims.x), xb = wrap(x0 + 1, cv.dims.x);
    const lengthType ya = wrap(y0, cv.dims.y), yb = wrap(y0 + 1, cv.dims.y);

    const RGB c00 = get(cv, {xa, ya, z}), c10 = get(cv, {xb, ya, z});
    const RGB c01 = get(cv, {xa, yb, z}), c11 = get(cv, {xb, yb, z});
    const auto mix = [](uint8_t a, uint8_t b, uint8_t t) {
        return static_cast<uint8_t>(a + (((static_cast<int32_t>(b) - a) * t) >> 8));
    };
    return RGB{mix(mix(c00.r, c10.r, fx), mix(c01.r, c11.r, fx), fy),
               mix(mix(c00.g, c10.g, fx), mix(c01.g, c11.g, fx), fy),
               mix(mix(c00.b, c10.b, fx), mix(c01.b, c11.b, fx), fy)};
}

/// How a sample outside the grid is read: wrapped around, or held at the edge.
enum class Edge : uint8_t {
    Wrap,    ///< the far side comes back around: seamless for a scroll, a tunnel, a torus flow
    Clamp,   ///< the edge pixel repeats: what a flow leaving the grid should smear against
};

/// Bilinear sample at a sub-pixel coordinate, holding the edge pixel outside the grid.
///
/// The Clamp half of `sampleWrap`. A flow that carries pixels off one side must not have them
/// reappear on the other, which is what Wrap does and what makes it wrong for a wind: the trail
/// would loop the panel instead of leaving it.
inline RGB sampleClamp(const Canvas& cv, pos_t x, pos_t y, lengthType z = 0) {
    if (cv.dims.x <= 0 || cv.dims.y <= 0) return RGB{0, 0, 0};
    const int32_t x0 = toPixel(x), y0 = toPixel(y);
    const uint8_t fx = static_cast<uint8_t>(x & (kSubOne - 1));
    const uint8_t fy = static_cast<uint8_t>(y & (kSubOne - 1));
    const auto clamp = [](int32_t v, lengthType n) {
        return static_cast<lengthType>(v < 0 ? 0 : (v >= n ? n - 1 : v));
    };
    const lengthType xa = clamp(x0, cv.dims.x), xb = clamp(x0 + 1, cv.dims.x);
    const lengthType ya = clamp(y0, cv.dims.y), yb = clamp(y0 + 1, cv.dims.y);
    const RGB c00 = get(cv, {xa, ya, z}), c10 = get(cv, {xb, ya, z});
    const RGB c01 = get(cv, {xa, yb, z}), c11 = get(cv, {xb, yb, z});
    const auto mix = [](uint8_t a, uint8_t b, uint8_t tt) {
        return static_cast<uint8_t>(a + (((static_cast<int32_t>(b) - a) * tt) >> 8));
    };
    return RGB{mix(mix(c00.r, c10.r, fx), mix(c01.r, c11.r, fx), fy),
               mix(mix(c00.g, c10.g, fx), mix(c01.g, c11.g, fx), fy),
               mix(mix(c00.b, c10.b, fx), mix(c01.b, c11.b, fx), fy)};
}

/// Sample under either edge rule, so a caller carries the choice as data rather than a branch.
inline RGB sampleEdge(const Canvas& cv, pos_t x, pos_t y, Edge edge, lengthType z = 0) {
    return edge == Edge::Wrap ? sampleWrap(cv, x, y, z) : sampleClamp(cv, x, y, z);
}

/// Move every pixel of `src` along a velocity field and write the result into `dst`.
///
/// Advection: the transport half of a flow, and the primitive a trail is made of. `rule` answers,
/// for a pixel, which way and how fast the medium is moving there; this walks BACKWARD along that
/// velocity and samples where the pixel must have come from, which is the standard stable form
/// (Stam, "Stable Fluids", SIGGRAPH 1999). Going backward rather than forward is what keeps it from
/// tearing: every destination pixel is written exactly once, so no gaps open where the field
/// diverges and nothing is written twice where it converges.
///
/// `dst` and `src` MUST be different planes. Reading and writing one buffer would sample pixels the
/// same pass had already moved, which smears along the walk order rather than along the flow: the
/// caller keeps a scratch plane and swaps, the ping-pong an effect owns.
///
/// The rule is called once per pixel and returns sub-pixel units per frame, so a velocity already
/// carries the frame's dt: the caller scales it, since only the caller knows the cadence.
template <typename Rule>
inline void advect(const Canvas& dst, const Canvas& src, Rule&& rule, Edge edge = Edge::Wrap) {
    if (!dst.data || !src.data) return;
    if (dst.dims.x != src.dims.x || dst.dims.y != src.dims.y) return;   // two shapes, no meaning
    for (lengthType z = 0; z < dst.dims.z; z++) {
        for (lengthType y = 0; y < dst.dims.y; y++) {
            for (lengthType x = 0; x < dst.dims.x; x++) {
                pos_t vx = 0, vy = 0;
                rule(x, y, z, vx, vy);
                // BACKWARD: where did what is here now come from? Hence the subtraction.
                const pos_t sx = toSub(x) - vx;
                const pos_t sy = toSub(y) - vy;
                pixel(dst, {x, y, z}, sampleEdge(src, sx, sy, edge, z));
            }
        }
    }
}

/// Advect a 16-BIT plane: the form a trail uses, and the reason it survives.
///
/// The Canvas vocabulary is 8-bit throughout (`get`, `pixel` and `sampleWrap` all speak `RGB`), so a
/// wide plane cannot borrow it: a Canvas over 16-bit data would read the high and low halves of one
/// channel as two different colors. This is the same walk against `uint16_t` samples, three per
/// light, with the bilinear blend done at full width so the transport does not quantize what the
/// decay is about to multiply. `n` is samples, `w`/`h`/`d` the geometry they are laid out in.
///
/// Edge::Clamp holds the border sample; Edge::Wrap brings the far side around.
template <typename Rule>
inline void advect16(uint16_t* dst, const uint16_t* src, lengthType w, lengthType h, lengthType d,
                     Rule&& rule, Edge edge = Edge::Wrap) {
    if (!dst || !src || w <= 0 || h <= 0 || d <= 0) return;
    const auto fold = [&](int32_t v, lengthType n) -> lengthType {
        if (edge == Edge::Wrap) { const int32_t m = v % n; return static_cast<lengthType>(m < 0 ? m + n : m); }
        return static_cast<lengthType>(v < 0 ? 0 : (v >= n ? n - 1 : v));
    };
    for (lengthType z = 0; z < d; z++) {
        for (lengthType y = 0; y < h; y++) {
            for (lengthType x = 0; x < w; x++) {
                pos_t vx = 0, vy = 0;
                rule(x, y, z, vx, vy);
                const pos_t sx = toSub(x) - vx, sy = toSub(y) - vy;      // BACKWARD, as advect()
                const int32_t x0 = toPixel(sx), y0 = toPixel(sy);
                const uint32_t fx = static_cast<uint32_t>(sx & (kSubOne - 1));
                const uint32_t fy = static_cast<uint32_t>(sy & (kSubOne - 1));
                const lengthType xa = fold(x0, w), xb = fold(x0 + 1, w);
                const lengthType ya = fold(y0, h), yb = fold(y0 + 1, h);
                const size_t slice = static_cast<size_t>(z) * h * w;
                const size_t i00 = (slice + static_cast<size_t>(ya) * w + xa) * 3;
                const size_t i10 = (slice + static_cast<size_t>(ya) * w + xb) * 3;
                const size_t i01 = (slice + static_cast<size_t>(yb) * w + xa) * 3;
                const size_t i11 = (slice + static_cast<size_t>(yb) * w + xb) * 3;
                const size_t o = (slice + static_cast<size_t>(y) * w + x) * 3;
                for (uint8_t c = 0; c < 3; c++) {
                    // Two lerps at full width: the top edge, the bottom edge, then between them.
                    const uint32_t a = src[i00 + c] + (((static_cast<int32_t>(src[i10 + c]) - src[i00 + c]) * static_cast<int32_t>(fx)) >> kSubShift);
                    const uint32_t b = src[i01 + c] + (((static_cast<int32_t>(src[i11 + c]) - src[i01 + c]) * static_cast<int32_t>(fx)) >> kSubShift);
                    dst[o + c] = static_cast<uint16_t>(a + (((static_cast<int32_t>(b) - static_cast<int32_t>(a)) * static_cast<int32_t>(fy)) >> kSubShift));
                }
            }
        }
    }
}

/// Combine two colors by taking the brighter channel: the "screen"/max operator feedback chains
/// use so a trail brightens rather than averaging away.
inline RGB combineMax(RGB a, RGB b) {
    return RGB{a.r > b.r ? a.r : b.r, a.g > b.g ? a.g : b.g, a.b > b.b ? a.b : b.b};
}


// ---- Signed distance fields --------------------------------------------------------------------
//
// An SDF answers "how far is this point from the shape's edge?": negative inside, zero on the edge,
// positive outside (Iñigo Quilez's 2D/3D distance-function catalog is the reference). One number
// then gives a filled shape, an anti-aliased edge, an outline (`|d| - width`), a glow (a falloff of
// d), and a smooth blend between shapes (`smin`), instead of a separate routine for each.
//
// It is also where dimension-generic stops being a slogan: `length(p) - r` is two points on a
// strand, a circle on a matrix and a sphere in a volume: the SAME code, because only the length
// changes. A rasteriser needs a different algorithm per dimension.
//
// **Distances are in sub-pixel units (pos_t, 24.8)**, so a shape can move and grow smoothly rather
// than jumping a whole pixel at a time.
//
// **Squared-first.** Measured on an ESP32-S3 at 128×128: the squared form costs ~14 cycles/pixel
// against ~108 for the true-distance form, because the ESP32 has no fast divide or sqrt. Every
// shape therefore has a `*Sq` variant that answers "inside/outside and by how much, squared", and
// effects should reach for it unless they need a real distance (outline width, linear falloff).

/// Squared distance from a point to a circle's center, minus the squared radius. Negative inside,
/// zero on the rim, positive outside: the sign and the ordering match the true SDF, so a threshold
/// test behaves identically without paying for a square root.
inline int32_t sdCircleSq(pos_t px, pos_t py, pos_t cx, pos_t cy, pos_t r) {
    // Work in whole sub-units squared; int64 because a 24.8 coordinate squared overflows int32 on a
    // large grid (a 16K-light strip is 4M sub-units, and 4M² needs 44 bits).
    const int64_t dx = px - cx, dy = py - cy;
    const int64_t d2 = dx * dx + dy * dy;
    const int64_t r2 = static_cast<int64_t>(r) * r;
    const int64_t diff = d2 - r2;
    // Saturate rather than wrap: a caller comparing against 0 only needs the sign, and a clamped
    // magnitude keeps the value usable as a falloff input.
    if (diff > INT32_MAX) return INT32_MAX;
    if (diff < INT32_MIN) return INT32_MIN;
    return static_cast<int32_t>(diff);
}

/// True signed distance to a circle's edge, in sub-pixel units. Costs a square root: use
/// `sdCircleSq` unless the actual distance is needed (an outline of a given width, a linear glow).
inline int32_t sdCircle(pos_t px, pos_t py, pos_t cx, pos_t cy, pos_t r) {
    const int64_t dx = px - cx, dy = py - cy;
    const uint64_t d2 = static_cast<uint64_t>(dx * dx + dy * dy);
    const uint32_t d = isqrt(static_cast<uint32_t>(d2 > UINT32_MAX ? UINT32_MAX : d2));
    return static_cast<int32_t>(d) - r;
}

/// Signed distance to an axis-aligned box centered at (cx, cy) with half-extents (bx, by).
///
/// Outside, this is the Chebyshev distance (the larger axis overshoot) rather than the Euclidean
/// one: it is exact along the faces, differs only near the corners, and costs no square root. For a
/// box on a light grid that difference is invisible, which is why the sqrt-free form is the only one
/// offered here.
inline int32_t sdBox(pos_t px, pos_t py, pos_t cx, pos_t cy, pos_t bx, pos_t by) {
    const pos_t qx = (px - cx < 0 ? cx - px : px - cx) - bx;
    const pos_t qy = (py - cy < 0 ? cy - py : py - cy) - by;
    const pos_t outX = qx > 0 ? qx : 0;
    const pos_t outY = qy > 0 ? qy : 0;
    const pos_t outside = outX > outY ? outX : outY;
    if (outside > 0) return outside;
    // Fully inside: the distance to the NEAREST face is the larger (least negative) overshoot.
    return qx > qy ? qx : qy;
}

/// Signed distance to a line segment a→b, minus `thickness`: a capsule, which is what a drawn line
/// with soft edges actually is. Projects the point onto the segment, clamps to its ends, then
/// measures. One square root; the projection itself is integer.
inline int32_t sdSegment(pos_t px, pos_t py, pos_t ax, pos_t ay, pos_t bx, pos_t by, pos_t thickness) {
    const int64_t pax = px - ax, pay = py - ay;
    const int64_t bax = bx - ax, bay = by - ay;
    const int64_t len2 = bax * bax + bay * bay;
    // Degenerate segment (a == b): fall back to the point distance, so a zero-length line is a dot
    // rather than a divide by zero.
    int64_t hx = pax, hy = pay;
    if (len2 > 0) {
        int64_t tNum = pax * bax + pay * bay;      // projection along the segment, scaled by len2
        if (tNum < 0) tNum = 0;
        if (tNum > len2) tNum = len2;
        hx = pax - (bax * tNum) / len2;
        hy = pay - (bay * tNum) / len2;
    }
    const uint64_t d2 = static_cast<uint64_t>(hx * hx + hy * hy);
    const uint32_t d = isqrt(static_cast<uint32_t>(d2 > UINT32_MAX ? UINT32_MAX : d2));
    return static_cast<int32_t>(d) - thickness;
}

/// Smooth minimum of two distances: the operator that makes two shapes flow into each other rather
/// than simply overlapping (Quilez, "smooth minimum"). `k` is the blend radius in sub-pixel units:
/// 0 gives a hard union (a plain `min`), larger values a longer merge.
///
/// This is metaballs generalised: the classic inverse-square blob field is one look, whereas `smin`
/// blends ANY pair of shapes: a circle into a box, a segment into a circle.
inline int32_t smin(int32_t a, int32_t b, int32_t k) {
    if (k <= 0) return a < b ? a : b;
    // Polynomial smooth min: h = clamp(0.5 + 0.5*(b-a)/k), mix(b, a, h) - k*h*(1-h).
    const int32_t diff = b - a;
    int32_t h = 128 + (static_cast<int64_t>(diff) * 128) / k;   // 0..256 in 8-bit fixed point
    if (h < 0) h = 0;
    if (h > 256) h = 256;
    // Both terms widen to 64 bits before multiplying. `a - b` is a difference of two distances, which
    // for saturated inputs already exceeds int32; and `k * h * (256 - h)` overflows int32 once k
    // passes ~131000 sub-units (512 pixels), which a control-driven blend radius reaches on a large
    // fixture. Either wrap makes smin return a value LARGER than both inputs, inverting the blend
    // it exists to produce.
    const int64_t mixed = b + ((static_cast<int64_t>(a) - b) * h) / 256;
    const int64_t bump  = (static_cast<int64_t>(k) * h * (256 - h)) / (256 * 256);
    return static_cast<int32_t>(mixed - bump);
}

/// Turn a signed distance into coverage: 255 well inside, 0 well outside, a ramp across the edge.
/// This is the anti-aliasing an SDF gives for free: the pixel is lit in proportion to how much of
/// it the shape covers, so an edge reads as smooth instead of stepped.
///
/// The ramp is one pixel wide by default, which matches how much area a boundary actually crosses.
inline uint8_t coverage(int32_t d, pos_t edge = kSubOne) {
    if (edge <= 0) return d <= 0 ? 255 : 0;
    if (d <= -edge) return 255;
    if (d >= edge) return 0;
    // Map [-edge, +edge] onto [255, 0].
    return static_cast<uint8_t>(((edge - d) * 255) / (2 * edge));
}


// --- Filled shapes with soft edges -------------------------------------------------------------
//
// `circle` and `fillCircle` above are Bresenham: whole pixels, hard edges, the right answer for a
// ring outline. These are the SDF forms, which take sub-pixel centers and radii and shade the
// boundary by coverage, so a small disc can sit between pixels and a growing one does not jump a
// whole cell at a time. On a 16x16 panel that difference is the whole difference between a blob
// that moves and a blob that stutters.

/// A filled disc with an anti-aliased edge, additive so overlapping discs brighten.
///
/// Coverage per pixel from the signed distance, over the bounding box only: a disc of radius r
/// touches (2r+2)^2 pixels however large the grid is, so this costs the shape rather than the frame.
inline void disc(const Canvas& cv, pos_t cx, pos_t cy, pos_t r, RGB c, lengthType z = 0) {
    if (r <= 0) return;
    const lengthType x0 = toPixel(cx - r) - 1, x1 = toPixel(cx + r) + 1;
    const lengthType y0 = toPixel(cy - r) - 1, y1 = toPixel(cy + r) + 1;
    for (lengthType y = y0; y <= y1; y++) {
        for (lengthType x = x0; x <= x1; x++) {
            // The pixel's CENTER against the edge: sampling the corner biases the shape half a
            // pixel up and left, which shows as a disc that drifts as it grows.
            const int32_t d = sdCircle(toSub(x) + kSubOne / 2, toSub(y) + kSubOne / 2, cx, cy, r);
            const uint8_t cov = coverage(d);
            if (cov == 0) continue;
            addPixel(cv, {x, y, z}, RGB{scale8(c.r, cov), scale8(c.g, cov), scale8(c.b, cov)});
        }
    }
}

/// A circle OUTLINE of a given stroke width, centered on the radius: `thickness` of one pixel
/// (`kSubOne`) draws a one-pixel ring. Sub-pixel and antialiased like `disc`.
///
/// A shape of its own rather than two discs subtracted, and the difference is visible: subtracting
/// leaves the INNER edge hard, because the cut-out disc's coverage is not blended, only removed. A
/// ring drawn from its own distance band gets both edges antialiased, which is what lets a thin
/// ring stay smooth while it grows.
inline void ring(const Canvas& cv, pos_t cx, pos_t cy, pos_t r, pos_t thickness, RGB c,
                 lengthType z = 0) {
    if (r <= 0 || thickness <= 0) return;
    const pos_t half = thickness / 2;
    const lengthType x0 = toPixel(cx - r - half) - 1, x1 = toPixel(cx + r + half) + 1;
    const lengthType y0 = toPixel(cy - r - half) - 1, y1 = toPixel(cy + r + half) + 1;
    for (lengthType y = y0; y <= y1; y++) {
        for (lengthType x = x0; x <= x1; x++) {
            // sdCircle gives the distance to the circle's LINE (signed, negative inside). The ring
            // is the band within `half` of that line, so its own distance is how far outside the
            // band the pixel sits: |d| - half.
            const int32_t d = sdCircle(toSub(x) + kSubOne / 2, toSub(y) + kSubOne / 2, cx, cy, r);
            const int32_t off = (d < 0 ? -d : d) - half;
            const uint8_t cov = coverage(off);
            if (cov == 0) continue;
            addPixel(cv, {x, y, z}, RGB{scale8(c.r, cov), scale8(c.g, cov), scale8(c.b, cov)});
        }
    }
}

/// A line with WIDTH and sub-pixel endpoints, antialiased along both edges and round-capped.
///
/// Distinct from `line` rather than an overload of it, because they answer different questions.
/// `line` walks whole pixels by Bresenham and is the cheap one, right for a wire-frame or a
/// scribble. This one places a stroke of real width at a fractional position, which is what a clock
/// hand or a spoke needs: on a 16-pixel panel a Bresenham hand jumps a whole pixel at a time and
/// reads as broken, where this moves smoothly because brightness carries the fraction.
inline void strokeLine(const Canvas& cv, pos_t x0, pos_t y0, pos_t x1, pos_t y1, pos_t thickness,
                       RGB c, lengthType z = 0) {
    if (thickness <= 0) return;
    const pos_t half = thickness / 2;
    const int64_t dx = static_cast<int64_t>(x1) - x0, dy = static_cast<int64_t>(y1) - y0;
    const int64_t lenSq = dx * dx + dy * dy;
    // A zero-length stroke is a dot, which is what a fully retracted hand should still draw.
    if (lenSq == 0) { disc(cv, x0, y0, half, c, z); return; }

    const pos_t loX = (x0 < x1 ? x0 : x1) - half, hiX = (x0 > x1 ? x0 : x1) + half;
    const pos_t loY = (y0 < y1 ? y0 : y1) - half, hiY = (y0 > y1 ? y0 : y1) + half;
    for (lengthType y = toPixel(loY) - 1; y <= toPixel(hiY) + 1; y++) {
        for (lengthType x = toPixel(loX) - 1; x <= toPixel(hiX) + 1; x++) {
            const pos_t px = toSub(x) + kSubOne / 2, py = toSub(y) + kSubOne / 2;
            // Project the pixel onto the segment, CLAMPED to its ends: without the clamp the
            // stroke would run on along the infinite line, and the two caps would be square rather
            // than round. `t` is the position along the segment, 0..kSubOne.
            const int64_t vx = static_cast<int64_t>(px) - x0, vy = static_cast<int64_t>(py) - y0;
            int64_t t = ((vx * dx + vy * dy) * kSubOne) / lenSq;
            if (t < 0) t = 0;
            if (t > kSubOne) t = kSubOne;
            const pos_t nx = static_cast<pos_t>(x0 + ((dx * t) >> kSubShift));
            const pos_t ny = static_cast<pos_t>(y0 + ((dy * t) >> kSubShift));
            // Distance to that nearest point, then the same edge rule every shape here uses:
            // sdCircle with the stroke's half-width IS "how far outside the stroke am I".
            const uint8_t cov = coverage(sdCircle(px, py, nx, ny, half));
            if (cov == 0) continue;
            addPixel(cv, {x, y, z}, RGB{scale8(c.r, cov), scale8(c.g, cov), scale8(c.b, cov)});
        }
    }
}

/// The volumetric form: a filled sphere, shaded the same way.
///
/// Separate rather than a defaulted depth on `disc`, because the cost differs by an order: a sphere
/// touches (2r+2)^3 pixels. A caller on a panel should get the cheap one without thinking about it.
inline void sphere(const Canvas& cv, pos_t cx, pos_t cy, pos_t cz, pos_t r, RGB c) {
    if (r <= 0) return;
    const lengthType x0 = toPixel(cx - r) - 1, x1 = toPixel(cx + r) + 1;
    const lengthType y0 = toPixel(cy - r) - 1, y1 = toPixel(cy + r) + 1;
    const lengthType z0 = toPixel(cz - r) - 1, z1 = toPixel(cz + r) + 1;
    for (lengthType z = z0; z <= z1; z++) {
        for (lengthType y = y0; y <= y1; y++) {
            for (lengthType x = x0; x <= x1; x++) {
                const int64_t dx = toSub(x) + kSubOne / 2 - cx;
                const int64_t dy = toSub(y) + kSubOne / 2 - cy;
                const int64_t dz = toSub(z) + kSubOne / 2 - cz;
                const uint64_t d2 = static_cast<uint64_t>(dx * dx + dy * dy + dz * dz);
                const uint32_t dist = isqrt(static_cast<uint32_t>(d2 > UINT32_MAX ? UINT32_MAX : d2));
                const uint8_t cov = coverage(static_cast<int32_t>(dist) - r);
                if (cov == 0) continue;
                addPixel(cv, {x, y, z}, RGB{scale8(c.r, cov), scale8(c.g, cov), scale8(c.b, cov)});
            }
        }
    }
}

// --- Rendering below the output resolution -----------------------------------------------------
//
// The cost of a field effect is per light, so the cheapest way to afford one on a large fixture is
// to compute FEWER lights and interpolate the rest. A field is smooth by construction, which is
// exactly the property that makes this nearly free visually: at half resolution a noise field
// carries a quarter of the samples and the eye cannot tell, because the values in between were
// always going to be close to their neighbors.
//
// This is the `fieldScale` lever. Its companion is `fieldRate`, which is not a primitive: an effect
// updates its field every N frames and lets the oscillators advance every frame, so the motion
// stays smooth while the expensive part runs less often.
//
// **What it is worth depends on the field's cost per sample, and only that.** The stretch itself is
// a fixed price per OUTPUT light (measured at 1.55 ns a sample, which is the bilinear arithmetic's
// own floor), so it saves nothing on a field that was already cheap. Measured on a 64x64 layer:
//
//   | field | full | half | quarter |
//   |---|---|---|---|
//   | 2-octave fbm (1 noise sample) | 59 us | 34 us (1.7x) | 23 us (2.6x) |
//   | curl (4 noise samples) | 212 us | 71 us (3.0x) | 32 us (6.6x) |
//
// So this is a lever for the expensive fields, the ones an S3 cannot otherwise afford, and a
// caller reaching for it on a cheap one is paying the stretch for almost nothing.

/// One destination column's blend: the two source columns and the weight between them.
struct UpscaleTap { uint16_t a, b; uint16_t w; };

/// Bilinearly stretch a smaller 16-bit plane over a larger one.
///
/// `src` is `sw x sh x sd` samples, three per light; `dst` is `dw x dh x dd`. Both are the effect's
/// own planes, so this takes raw pointers rather than a Canvas: a field at half resolution is not a
/// fixture and has no channel count of its own.
///
/// The z axis is interpolated too when both planes have depth, so a volumetric field can be
/// computed on a coarse cube. A plane with `sd == 1` is stretched flat across the destination's
/// depth instead, which is what a 2D field on a 3D fixture wants and costs no z work at all.
///
/// `taps` is the caller's scratch, `dw` entries: the per-column blend table, hoisted out of the
/// inner loop. It belongs to the caller because this runs from tick(), where neither the heap nor
/// a large frame is available. An earlier version held it as a 4096-entry local, which is 24 KB on
/// a stack the ESP32 gives 12 KB (CONFIG_ESP_MAIN_TASK_STACK_SIZE), so the first stretched frame
/// would have overflowed it. Pass fewer than `dw` entries and the call does nothing.
inline void upscale16(uint16_t* dst, lengthType dw, lengthType dh, lengthType dd,
                      const uint16_t* src, lengthType sw, lengthType sh, lengthType sd,
                      UpscaleTap* taps, size_t tapCount) {
    if (!dst || !src || dw <= 0 || dh <= 0 || dd <= 0 || sw <= 0 || sh <= 0 || sd <= 0) return;
    if (!taps || tapCount < static_cast<size_t>(dw)) return;
    // Map destination center to source in 16.16, so the edges land on the edges rather than
    // drifting half a cell: (d + 0.5) * s / D - 0.5, the standard alignment.
    //
    // The mapping is computed ONCE per axis, not per pixel: it needs a 64-bit divide by a runtime
    // extent, and doing that three times per output sample made the upscale cost more than the
    // field it was meant to save (measured: a flat 22 us against a field that dropped from 59 to
    // 15). The x row is a small table, y and z are stepped, so the inner loop is adds and shifts.
    const auto axis = [](lengthType d, lengthType dn, lengthType sn) -> int32_t {
        if (dn <= 1) return 0;
        const int64_t num = (static_cast<int64_t>(d) * 2 + 1) * sn - dn;
        return static_cast<int32_t>((num << 15) / dn);          // 16.16, may be negative at the edge
    };
    const auto clamp = [](int32_t v, lengthType n) -> size_t {
        return static_cast<size_t>(v < 0 ? 0 : (v >= n ? n - 1 : v));
    };
    // One entry per destination column: the two source columns it blends and the weight between.
    UpscaleTap* row = taps;
    for (lengthType x = 0; x < dw; x++) {
        const int32_t fx = axis(x, dw, sw);
        row[x] = {static_cast<uint16_t>(clamp(fx >> 16, sw)),
                  static_cast<uint16_t>(clamp((fx >> 16) + 1, sw)),
                  static_cast<uint16_t>(fx & 0xFFFF)};
    }
    const bool interpZ = sd > 1;
    for (lengthType z = 0; z < dd; z++) {
        const int32_t fz = interpZ ? axis(z, dd, sd) : 0;
        const int32_t z0 = fz >> 16;
        const uint32_t wz = static_cast<uint32_t>(fz & 0xFFFF);
        const size_t za = clamp(z0, sd), zb = interpZ ? clamp(z0 + 1, sd) : za;
        for (lengthType y = 0; y < dh; y++) {
            const int32_t fy = axis(y, dh, sh);
            const int32_t y0 = fy >> 16;
            const uint32_t wy = static_cast<uint32_t>(fy & 0xFFFF);
            const size_t ya = clamp(y0, sh), yb = clamp(y0 + 1, sh);
            for (lengthType x = 0; x < dw; x++) {
                const UpscaleTap tap = row[x];
                const uint32_t wx = tap.w;
                const size_t xa = tap.a, xb = tap.b;
                const size_t slA = za * sh * sw, slB = zb * sh * sw;
                const size_t o = ((static_cast<size_t>(z) * dh + y) * dw + x) * 3;
                for (uint8_t c = 0; c < 3; c++) {
                    const auto at = [&](size_t sl, size_t yy, size_t xx) -> uint32_t {
                        return src[(sl + yy * sw + xx) * 3 + c];
                    };
                    // SIGNED difference: `b - a` on unsigned wraps whenever the field descends,
                    // and a wrapped row blended against an unwrapped one lands far outside the
                    // input range (a 2x2 saddle of 0 and 100 produced 98354). A noise field is a
                    // saddle almost everywhere, so this lit whole cells at full brightness.
                    const auto lerp = [](uint32_t a, uint32_t b, uint32_t w) -> uint32_t {
                        return static_cast<uint32_t>(static_cast<int64_t>(a)
                             + (((static_cast<int64_t>(b) - static_cast<int64_t>(a))
                                 * static_cast<int64_t>(w)) >> 16));
                    };
                    // Two rows of the near slice, then the far one, then between them.
                    const uint32_t n0 = lerp(at(slA, ya, xa), at(slA, ya, xb), wx);
                    const uint32_t n1 = lerp(at(slA, yb, xa), at(slA, yb, xb), wx);
                    uint32_t v = lerp(n0, n1, wy);
                    if (interpZ) {
                        const uint32_t f0 = lerp(at(slB, ya, xa), at(slB, ya, xb), wx);
                        const uint32_t f1 = lerp(at(slB, yb, xa), at(slB, yb, xb), wx);
                        v = lerp(v, lerp(f0, f1, wy), wz);
                    }
                    dst[o + c] = static_cast<uint16_t>(v);
                }
            }
        }
    }
}

// --- Narrowing 16-bit state to the wire -------------------------------------------------------
//
// Everything above computes wider than it writes. A field is 16-bit, a trail plane is 16-bit, and
// the wire is a byte, so somewhere the low half is thrown away. Doing that by `>> 8` is what makes
// a slow gradient band: measured on a dark 2%-wide ramp across 64 lights, the 16-bit values hold 64
// distinct levels and the truncation leaves 6.
//
// Dithering does not add levels. It moves the error somewhere the eye integrates it away:
//
//   - ORDERED (a 4x4 Bayer matrix, no state) spreads the error over SPACE. The mean lands right
//     (28.14 against 28.01 ideal, versus truncation's 27.59) and the band edge dissolves into a
//     texture. Free, and correct on a still image.
//   - TEMPORAL (one byte of carry per channel) spreads it over TIME: the remainder of this frame's
//     truncation is added to the next, so a pixel alternates between two levels in the proportion
//     its true value asks for. Measured over 32 frames, eight neighbors a fraction of a byte apart
//     resolve to 25.59, 25.75, 25.91, 26.06 ... where truncation reports 25, 25, 25, 26. This is
//     what makes a slow fade smooth rather than stepped, and it is the reason a 16-bit pipeline is
//     worth having at all on 8-bit LEDs.
//
// Temporal needs a byte of state per channel, which the caller owns: the state must persist across
// frames and belongs to whoever owns the plane. Ordered needs none, which is why it is the fallback
// for a caller with nowhere to keep it.

/// How the low half of a 16-bit sample is disposed of on the way to a byte.
enum class Dither : uint8_t {
    None,      ///< truncate: the fastest, and what bands
    Ordered,   ///< a 4x4 Bayer threshold on the pixel's position: stateless, spatial
    Temporal,  ///< carry the error into the next frame: needs a byte per channel, and is smoothest
};

/// The 4x4 Bayer matrix, scaled to a 0..255 threshold. The standard recurrence, in its usual order.
inline constexpr uint8_t kBayer4[16] = {
      8, 136,  40, 168,
    200,  72, 232, 104,
     56, 184,  24, 152,
    248, 120, 216,  88,
};

/// Narrow one 16-bit sample to a byte, dithered.
///
/// `carry` is the caller's per-channel state and is READ AND WRITTEN under Temporal; it is ignored
/// by the other modes, so a caller with no state passes a dummy. `x`/`y` position the Bayer
/// threshold under Ordered.
inline uint8_t quantize(uint16_t v, Dither mode, uint8_t& carry,
                        lengthType x = 0, lengthType y = 0, lengthType z = 0) {
    switch (mode) {
        case Dither::Ordered: {
            // z ROTATES the matrix rather than indexing a third dimension. A 4x4x4 table would be
            // the textbook answer and is the wrong trade here: it is 4x the constant data for a
            // pattern the eye never resolves in depth, and a volume's slices only need to avoid
            // sharing one threshold, not to be independently optimal. The rotation costs an add on
            // an index that is already being computed, so a panel (z always 0) pays nothing.
            const size_t cell = (static_cast<size_t>(y & 3) * 4) + (x & 3) + (static_cast<size_t>(z & 3) * 5);
            const uint8_t thr = kBayer4[cell & 15];
            const uint8_t hi = static_cast<uint8_t>(v >> 8);
            // Round up when the discarded low byte beats this pixel's threshold. Saturating, so a
            // sample already at full stays there rather than wrapping to black.
            return (static_cast<uint8_t>(v & 0xFF) > thr && hi < 255) ? static_cast<uint8_t>(hi + 1) : hi;
        }
        case Dither::Temporal: {
            // The remainder this frame could not express is added to the next, so the sequence of
            // bytes averages to the true value. One byte of carry is enough: the error is always
            // under one step, and letting it saturate rather than wrap keeps a bright pixel bright.
            const uint32_t x16 = static_cast<uint32_t>(v) + carry;
            const uint32_t hi = x16 >> 8;
            const uint8_t out = static_cast<uint8_t>(hi > 255 ? 255 : hi);
            const uint32_t used = static_cast<uint32_t>(out) << 8;
            carry = static_cast<uint8_t>(x16 > used ? (x16 - used > 255 ? 255 : x16 - used) : 0);
            return out;
        }
        case Dither::None:
        default:
            return static_cast<uint8_t>(v >> 8);
    }
}

/// A 16-bit plane onto the canvas: the one narrowing step, dithered.
///
/// Three effects held a copy of this loop and they disagreed, which is the reason it is here: two
/// dithered and one truncated, so the same slow fade banded in one effect and not the others. The
/// `carry` plane is the dither's per-channel error, `w*h*d*3` bytes sized by the caller's prepare();
/// pass nullptr for none and the narrowing truncates, which is what a plane with no fade wants.
inline void blit16(const Canvas& cv, const uint16_t* p, lengthType w, lengthType h, lengthType d,
                   uint8_t* carry) {
    if (!p) return;
    std::size_t i = 0;
    uint8_t dummy = 0;
    for (lengthType z = 0; z < d; z++)
        for (lengthType y = 0; y < h; y++)
            for (lengthType x = 0; x < w; x++, i += 3) {
                const auto q = [&](std::size_t c) {
                    uint8_t& st = carry ? carry[i + c] : dummy;
                    return quantize(p[i + c], carry ? Dither::Temporal : Dither::None, st, x, y, z);
                };
                pixel(cv, {x, y, z}, RGB{q(0), q(1), q(2)});
            }
}


// --- Velocity rules ----------------------------------------------------------------------------
//
// A velocity rule answers, for one point, which way the medium is moving there. They are plain
// functions rather than objects because none of them holds state: everything that varies over time
// arrives as an argument, so an effect can drive one from a control, an oscillator or audio without
// the rule knowing. `advect` takes any callable of this shape, so these are conveniences, not a
// closed set: an effect with its own idea writes a lambda.
//
// All return sub-pixels PER CALL, so the caller has already folded in the frame's dt. Prior art:
// the flow vocabulary of 4wheeljive's FlowFields (from a Stefan Petrick concept) and, for curl,
// Bridson 2007.

/// A steady wind: everything moves the same way at the same speed.
inline void flowWind(angle16 direction, int32_t speed, pos_t& vx, pos_t& vy) {
    vx = static_cast<pos_t>((static_cast<int32_t>(cos16(direction)) * speed) >> 15);
    vy = static_cast<pos_t>((static_cast<int32_t>(sin16(direction)) * speed) >> 15);
}

/// Out from a center, or into it when `speed` is negative. The fountain and the drain.
inline void flowRadial(lengthType x, lengthType y, lengthType cx, lengthType cy,
                       int32_t speed, pos_t& vx, pos_t& vy) {
    const int32_t dx = static_cast<int32_t>(x) - cx, dy = static_cast<int32_t>(y) - cy;
    if (dx == 0 && dy == 0) { vx = vy = 0; return; }        // the center has no direction to go
    const angle16 a = atan16(dy, dx);
    flowWind(a, speed, vx, vy);
}

/// Around a center and outward at once: the spiral, which is the two above added.
inline void flowSpiral(lengthType x, lengthType y, lengthType cx, lengthType cy,
                       int32_t angular, int32_t radial, pos_t& vx, pos_t& vy) {
    const int32_t dx = static_cast<int32_t>(x) - cx, dy = static_cast<int32_t>(y) - cy;
    if (dx == 0 && dy == 0) { vx = vy = 0; return; }
    const angle16 a = atan16(dy, dx);
    pos_t rx, ry, tx, ty;
    flowWind(a, radial, rx, ry);                            // outward
    flowWind(static_cast<angle16>(a + 16384), angular, tx, ty);   // and a quarter turn from it
    vx = rx + tx;
    vy = ry + ty;
}

}  // namespace mm::draw
