# Plan — Stage 1 (palette) of the MoonLight migration

The first executable slice of the [migration plan](../Plan-20260630%20-%20MoonLight%20migration%20%28multi-stage%29.md): the palette foundation. Design already decided in moonlight-palettes-data.md; this plan is the file split + the implementation specifics. The shared **primitive library** (beat/noise/blend/draw) and the **GoL re-port** are *separate* slices of Stage 1, planned + committed after this — palette is the load-bearing one, done first and alone so it's reviewable.

## What ships

1. **`src/light/Palette.h`** — a light-domain header (sibling of `light_types.h`), holding:
   - **`Palette`** — the active palette: **16 RGB entries** (the `CRGBPalette16` model, recognisable name carried, our implementation on `RGB`/`scale8`). 48 bytes.
   - **gradient-stop → 16-entry expansion** (`fromGradient(const uint8_t* stops, size_t n)`): the textbook two-point lerp across the stop list, sampling 16 evenly-spaced positions. Off the hot path (called on selection).
   - **`RGB colorFromPalette(const Palette& p, uint8_t index, uint8_t brightness = 255)`** — the per-light lookup: map `index` (0–255, wraps) to a position across the 16 entries, blend the two bracketing entries with `scale8`, apply `brightness` with `scale8`. Integer-only, hot-path-cheap.
   - **Built-in palette set** — the gradient `{pos,R,G,B}` definitions from the captured data as flash `constexpr`, plus the trivially-generated ones (rainbow via `hsvToRgb`, and a few solids). A `kPaletteNames[]` / `kPaletteCount` parallel to feed the select control. Start with a curated subset (~12–16: rainbow, party, ocean, lava, forest, heat, a couple of the named gradients) — not all ~54; the rest are data we can add later without design change.
2. **`Palettes::active()`** — the static seam effects read (the `AudioModule::latestFrame()` pattern): a `Palettes` holder with a static `const Palette* active()` + `setActive(index)` that expands the selected built-in into the live `Palette`. Lives in `Palette.h`.
3. **Drivers wiring** — a `palette` **select** control on the `Drivers` container (beside `brightness`/`lightPreset`), index into `kPaletteNames`. `Drivers::onUpdate` on a `palette` change calls `Palettes::setActive(index)` (rebuild the 16-entry lookup — cheap, like the `correction_.rebuild` it sits next to; `controlChangeTriggersBuildState` stays false, no pipeline realloc). `setup()` sets the initial active palette.
4. **Migrate `PlasmaPaletteEffect`** — replace its hard-coded `static constexpr RGB palette_[256]` with `colorFromPalette(Palettes::active(), idx)`. Proves the seam end-to-end on a real effect + removes a 256-entry duplicate (subtraction). (Its current fixed fire-ocean look changes to "whatever palette is active" — that's the point; the effect becomes palette-driven like its MoonLight original.)

## Decided (from the design doc — not re-opened here)

- 16-entry model, interpolate at lookup (not a 256 table); hard-swap on select (crossfade backlogged); global active palette owned by Drivers; `Palette` is an interface shape so a later **MoonLivePalette** (dynamic, script-authored) drops into the same `colorFromPalette` seam — Stage 1 builds only the gradient case but leaves `colorFromPalette` dispatchable.
- Naming: `Palette`, `colorFromPalette`, the `CRGBPalette16` model — recognisable names carried, FastLED credited (README + the spec's Prior art), implementation ours.

## Files

- **New:** `src/light/Palette.h`; `docs/moonmodules/light/Palette.md` (spec — the `Palette` type, `colorFromPalette` contract, the built-in set, the active-palette seam, Prior art crediting FastLED's gradient-palette model); `test/unit/light/unit_Palette.cpp`.
- **Edit:** `src/light/drivers/Drivers.h` (the `palette` select + onUpdate/setup); `src/light/effects/PlasmaPaletteEffect.h` (use the shared palette); `test/CMakeLists.txt`; the migration plan (mark palette slice landing).

## Riskiest parts

1. **The expansion + lookup must be correct *and* cheap** — pin both with tests (endpoints exact, a mid-gradient color interpolates, the wheel wraps at 255→0, brightness folds correctly). It runs per-light, so eyeball the KPI tick after wiring PlasmaPalette.
2. **`colorFromPalette` dispatchable without a per-pixel cost** — Stage 1 has only the gradient case, so it's a direct call now; the *interface shape* (so MoonLivePalette slots in later) must not impose a per-pixel branch today. Keep it a plain function over the 16-entry `Palette`; the static/dynamic dispatch is a later per-frame concern, not built now.
3. **PlasmaPalette visual change** — it goes from a fixed palette to the active one; confirm it still looks good on the default palette and that the effect's index math still maps sensibly.

## Verification

Desktop build (-Werror); `ctest` incl. `unit_Palette` (expand/lookup/wrap/brightness) + the existing PlasmaPalette test still green; scenarios; spec-check (new `Palette.md`); ESP32 build; KPI (watch the per-light cost — PlasmaPalette is the canary). Bench: on desktop, switch the `palette` control and confirm PlasmaPalette (and the preview) recolors live.

## Out of scope (later Stage-1 slices / later stages)

- The primitive library (beat/noise/blend/draw) — next Stage-1 slice.
- GoL re-port — next Stage-1 slice.
- The full ~54 palette set, MoonLivePalette (dynamic), crossfade, the per-library doc pages (Stage 2).
