# Plan — Stage 1 primitive library (math8 + noise + draw + blend)

The remaining foundation of [MoonLight migration Stage 1](../Plan-20260630%20-%20MoonLight%20migration%20%28multi-stage%29.md) (palette + tags-legend already shipped in `d00559c`). Builds the shared, hot-path-tuned integer primitives every migrated effect (Stage 3+) will call, so each effect stays short by leaning on one recognisable set instead of re-rolling beat/noise/blend/draw per effect.

**Prior art:** FastLED — the canonical 8-bit-fixed-point LED library. We carry its *ideas and recognisable names* (`beatsin8`, `inoise8`, `qadd8`, `nscale8`, `random8`, `fadeToBlackBy`, `blend`) and write our own implementation against our architecture, crediting FastLED at each file's header. FastLED's own split is the model: `lib8tion` (math+timing+random), `noise` (inoise), `colorutils` (blend/fade), `hsv2rgb` (color) — draw/Bresenham lives in its 2D/matrix add-ons, not core.

## File split (decided 2026-06-30) — split-by-concern, math in core / draw in light

**Core (`src/core/`)** — domain-neutral integer math:
- **`color.h`** (slimmed) — keeps `RGB`, `hsvToRgb`, `scale8` (the color surface). `sin8`/`cos8`/`atan2_8`/`dist8` MOVE OUT (they're trig/geometry, not color).
- **`math8.h`** (new) — the `lib8tion` surface: `sin8`/`cos8`/`triwave8` (moved from color.h), `beatsin8`/`beatsin16`/`beat8` (timing, on `sin8`+`elapsed()`), `qadd8`/`qsub8`/`nscale8`, `map8`, a small seedable PRNG — the **`Random8` class** (`next8()`/`next16()`/`below(n)`/`below(min,max)`, not free `random8`/`random16` functions, so each effect owns an independent reproducible stream), `atan2_8`/`dist8` (moved from color.h). *(As shipped, `beatsin8` is the FastLED 5-arg form `(bpm, ms, low, high, timebase, phase)`.)*
- **`noise.h`** (new) — `inoise8` 1D/2D/3D (promotes + generalises `NoiseEffect`'s existing hash into the textbook value/Perlin noise; the effect then calls it).

**Light (`src/light/`)** — operates on the light `Buffer`/`Coord3D`:
- **`draw.h`** (new) — `drawPixel(buf, Coord3D, RGB)` / `drawLine(buf, Coord3D a, Coord3D b, RGB)` working 1D→3D against the `Buffer` (integer Bresenham). Geometry lives once (the "core absorbs the hard part" principle), light-domain because it touches the light Buffer. Circle/fill are a later add.
- **`Palette.h`** (extend) — fold in `fadeToBlackBy(RGB&, amt)` and `blend(RGB, RGB, amt)` next to `colorFromPalette` (FastLED's `colorutils` shape; RGB-blend ops belong with the palette/color-lookup file, not a new file for two functions).

Net: 2 new core files + 1 new light file + 2 edits (color.h slim, Palette.h extend). ~20 primitives across 4–5 focused files — recognisable to any FastLED/embedded dev in 30s, no per-function explosion, color.h cleaned to color-only.

## The `color.h` move (the churn this incurs)

`sin8`/`cos8`/`atan2_8`/`dist8` move `color.h` → `math8.h`. 18 files include `core/color.h`; the ones using sin8/dist8 (effects: Wave, Spiral, Plasma, Metaballs, DistortionWaves, Ripples, Sine…) add `#include "core/math8.h"`. `color.h` includes nothing new. Mechanical, caught by -Werror (missing symbol) if an include is missed.

## Files

- **New:** `src/core/math8.h`, `src/core/noise.h`, `src/light/draw.h`.
- **Edit:** `src/core/color.h` (remove sin8/cos8/atan2_8/dist8 → math8.h; keep RGB/hsvToRgb/scale8), `src/light/Palette.h` (add fadeToBlackBy/blend), every effect that used sin8/dist8 (add the math8 include), `NoiseEffect.h` (call `inoise8` instead of its inline hash — proves the promotion).
- **Tests (new):** `test/unit/core/unit_math8.cpp` (beatsin8 range + period, qadd8/qsub8 saturation, nscale8, random8 determinism+distribution, triwave8), `test/unit/core/unit_noise.cpp` (inoise8 determinism + smoothness + 1D/2D/3D bounds), `test/unit/light/unit_draw.cpp` (drawPixel in-bounds/clipped, drawLine endpoints + a known diagonal in 1D/2D/3D).
- **Docs:** a short `docs/coding-standards.md` or architecture note pointing effects at the primitive set (one reference, not per-primitive docs). No module `.md` (these are libraries, not MoonModules).

## Hard-rule conformance

- **Hot path:** all integer, LUT-backed where applicable (`sin8` already a 256-LUT; `inoise8` interpolates a hash, no float). No heap, no allocation. `random8` is a 1-line LCG/xorshift, not `std::rand`.
- **Platform boundary:** pure computation, no platform calls — lives outside `src/platform/`. `beatsin8` reads time via the existing `elapsed()`/`platform::millis()` seam the effects already use (passed in, not called inside core — confirm the cleanest signature in implementation: likely `beatsin8(bpm, low, high, timeMs)` so core stays time-source-agnostic).
- **Domain boundary:** math8/noise are domain-neutral (core); draw touches the light Buffer (light). No `#ifdef`.

## Verification

- Build -Werror (the color.h move surfaces any missed include immediately).
- `ctest`: each primitive pinned (ranges, saturation, determinism, endpoints) per the stage-1 exit criteria. Scenarios still green (NoiseEffect now via inoise8 must still render non-zero + vary).
- KPI: tick unchanged or better (primitives are the same work the effects already did, now shared — NoiseEffect's hash promotion should be perf-neutral).
- No P4/S3 regression (bench after, given the recent stack lesson — though these add no large members).

## Stage-1 exit (from the migration plan)

After this: palette ✅ (shipped), tags legend ✅ (shipped), primitives ✅ (here, unit-tested), doc-model ✅ (shipped early in Stage 2's compact pages). The one remaining Stage-1 item — **the GoL re-port** (the proof effect on palette+primitives) — is the NEXT plan after this, since it depends on `inoise8`/`random8`/`drawPixel` landing first.

## Out of scope

- GoL re-port (next plan — needs these primitives first).
- `drawCircle`/fill, font/glyph blitter (Stage 3e), `ease8`/gamma (add when an effect needs them — concrete-first).
- Touching `colorFromPalette` itself (shipped, working).
