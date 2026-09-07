# Plan: Black pixels (dark gaps) in Layouts

## Context

A user drives a continuous WS2812 panel/strand where some LEDs must stay DARK mid-run: data flows THROUGH the dark LEDs to reach lit ones beyond, but those positions show no light (a sealed panel with a spacer strip, a slat wall). The existing `start`/`count` window only selects one contiguous subset; it can't punch a hole mid-grid.

A first attempt put this in the DRIVER (a `g`/`+` gap syntax on the `ledsPerPin` control). That is being **abandoned**: it's the wrong layer (a driver text-parse), and the crippling flaw is the **preview cannot show the dark gaps** (the preview renders the light buffer, not the wire). The product owner's decision: solve it in **Layouts**, so the black positions live in the mapping and the preview shows the holes in place for free.

**Design model (settled with the PO):** the panel is HOLED, not collapsed. A 16x16 panel with columns 10-15 dark renders the FULL 16x16 effect; the lit LEDs (columns 0-9) show exactly the visible part of the picture at its true (x,y), and the dark LEDs are the part you can't see. Collapsing to a dense 10x16 would distort the geometry (a circle would squash leftward), which is wrong. So a black pixel is **a hole at its true coordinate in the full grid**, not a removed column.

## How the existing pipeline already supports this (traced, build on it)

The whole light pipeline funnels through one `forEachCoord` seam, and the holed model is the EXISTING sparse mechanism (the same one WheelLayout uses), applied to a grid:

- **`Layouts::forEachCoord`** ([Layouts.h:49](src/light/layouts/Layouts.h)) emits every physical position `(idx, x, y, z)`; `totalLightCount()` ([Layouts.h:39](src/light/layouts/Layouts.h)) = Sum of child `lightCount()` sizes the driver output buffer AND the preview.
- **`Layer` derives the physical box** as the bounding box of emitted coordinates ([Layer.h:107-115](src/light/layers/Layer.h)) and builds the LUT by folding each physical coordinate to a logical cell ([Layer.h:449-468](src/light/layers/Layer.h)); a coordinate the fold rejects is DROPPED (`if (!m->modifyLogical(pos)) return;`, [Layer.h:457](src/light/layers/Layer.h)) - it consumes a physical index but maps to no logical cell.
- **`blendMap` SCATTERS** logical to physical with `clearFirst` ([BlendMap.h:88,103](src/light/layers/BlendMap.h)): it clears the output buffer, then writes each logical light to its physical destination(s). A physical index that is no light's destination is NEVER written and stays black. No gather, no bogus read.
- **The LED driver clocks its window LINEARLY** ([ParallelLedDriver.h:899](src/light/drivers/ParallelLedDriver.h)): `src + (winStart_ + laneStart_ + sourceRow)`, every position in order, whatever color is there. It does NOT walk the LUT or skip positions - so an unmapped (black) position between mapped ones is clocked BLACK IN PLACE. Data flows through. This is exactly what a dark-gap strand needs.
- **`PreviewDriver` walks `forEachCoord`** ([PreviewDriver.h:432](src/light/drivers/PreviewDriver.h)) and emits each physical `idx`'s color; a black position reads its zero-init (black) buffer slot and draws dark AT ITS (x,y). The hole shows for free.

**Net:** the driver buffer is already physical-sized and zero-init; the scatter already leaves un-scattered positions black; the driver already clocks all positions; the preview already renders physical positions. The ONLY missing piece is a way for a layout to mark a physical/wire position as a GAP (a real physical pixel that must stay black, carrying no logical source), and for the buffer-copy step to honor that mark. Everything else falls out.

## The model: two kinds of pixel (the PO's framing)

A layout is a sequence of pixel emissions, of which there are now TWO kinds (instead of one). **Both are real physical pixels the driver clocks** - the difference is only whether a color reaches them:

- **`addPixel(x,y,z)`** - a WIRE position that maps to a logical cell. The scatter writes the cell's color here; the driver clocks it lit.
- **`addBlackPixel(x,y,z)`** - a WIRE position that is a GAP: **a physical pixel that must stay black**. The scatter writes nothing to it (it carries no logical source), so it stays black; the driver STILL clocks it (data flows through the physical LED to reach lit LEDs beyond); the preview draws it dark at (x,y).

The earlier framing "a black pixel has no physical pixel" was wrong: it DOES have a physical pixel (a real wire slot on a continuous strand - "physical LED, forced off"), it just must not receive a color. Both kinds ADVANCE the physical/wire index. This is the existing sparse-mapping shape, with the gap declared per-emission instead of emerging from a rejecting modifier.

### Realized as a `black` flag on the shared coordinate emission

`forEachCoord` is the one seam; a black pixel rides ON it as a trailing `bool black` (not a side-channel list, which would be a second source of truth that drifts):

- **[LayoutBase.h:31](src/light/layouts/LayoutBase.h):** `using CoordCallback = void(*)(void* ctx, nrOfLightsType idx, lengthType x, lengthType y, lengthType z, bool black);`
  - Justification at the introduction site (common-patterns-first): the black/lit distinction is co-located with the (x,y,z) it applies to, on the seam every layout and consumer already shares.

The signature change is mechanical across the fixed set of `CoordCallback` lambdas; each either forwards or ignores `black`:
- **[Layouts.h:62](src/light/layouts/Layouts.h)** wrapper: forward `black` through (keep `offset += layout->lightCount()`, physical/wire count, unchanged - black pixels ARE counted, they occupy wire slots).
- **[Layer.h:449](src/light/layers/Layer.h)** `onCoord` (the LUT fold): `if (black) return;` at the top - a black wire position gets no logical->physical mapping in the LUT (no cell scatters to it). It still consumed its physical index in the walk, so its wire slot exists.
- **[Layer.h:107](src/light/layers/Layer.h)** dimension lambda: ignore `black` (a black pixel still expands the physical bounding box - it occupies a real wire position at (x,y)).
- **[Layer.h:399](src/light/layers/Layer.h)** `isNaturalOrder` lambda: ignore `black` (see fast-path note).
- **[PreviewDriver.h:432](src/light/drivers/PreviewDriver.h)** and its coord-table builder (~:322): ignore `black` (the real coordinate already draws the hole; the black wire slot reads its zero-init color).

### Where the gap becomes black: the buffer-copy step

The PO's framing pins WHERE the black is applied: **the buffer-copy step** that copies the Layer's virtual buffer to the driver output buffer - that is `blendMap` ([BlendMap.h:54](src/light/layers/BlendMap.h)). It SCATTERS (walks logical cells, writes each to its physical destination), so a black wire slot - one no cell maps to - is simply never written. Because `blendMap` does `dst.clear()` first for the bottom layer ([BlendMap.h:88](src/light/layers/BlendMap.h)), that slot stays black. So on the common single-layer path the gap is honored FOR FREE by the existing clear; no new code in the copy. **To verify during implementation:** a black slot in a NON-bottom composited layer (a second Layer alpha/additive over the first) is not re-cleared, so confirm it either can't receive stale data or add the one guard in the copy loop (skip/zero a destination flagged as a gap). This is the "add it in the buffer-copying code" the PO called out - present only if the clear doesn't already cover the composite case.

## Authoring surface: a black-region control on GridLayout

GridLayout is COMPUTED (nested loops in `forEachCoord`), not literally a list of `addPixel`/`addBlackPixel` calls. Do NOT convert it to emit-based (that forks the layout model). Instead the loop DECIDES per cell whether that emission is the `addPixel` or the `addBlackPixel` kind, driven by a control - the same two-kinds model, expressed computationally.

- **[GridLayout.h](src/light/layouts/GridLayout.h):** add a `blackColumns` text control - a per-row set of x-ranges that are black (start with a SINGLE contiguous run, e.g. `"10-15"` or a `blackStart`/`blackCount` pair; empty = none). This covers the PO's stated slat-wall example exactly. Reuse the existing range-parse idiom (the `"start-end"` range shape already parsed by the pin-range / RegionModifier code), bounded and heap-free.
  - `lightCount()` (physical/wire count) stays `width*height*depth` - the wire clocks every cell including black (they are physical LEDs, forced off).
  - `forEachCoord` inner x-loop: compute `bool black = xInBlackSet(x)` per cell; call `cb(ctx, idx++, x, y, z, black)`. The physical `idx` advances for black cells too. Lit cells are `addPixel`, black cells are `addBlackPixel`.
  - No count-space split: the effect renders the full grid box; the fold simply produces fewer LUT entries (160) than physical/wire positions (256). The 96 black wire positions carry no LUT entry and stay dark.

Multi-run per row (`"3-5,10-15"`) and arbitrary 2D masks are deferred as later additive changes only if a real panel needs them (subtraction beats addition; the single-run form is the minimum that solves the example).

## Fast-path guard (the one correctness trap)

The dense-identity fast path ([Layer.h:359,364](src/light/layers/Layer.h), `dense = driverCount == boxCount`) must NOT be taken when black pixels exist: with black cells the physical bounding box volume `boxCount` still equals `driverCount` (black cells fill grid positions), so `dense` would be TRUE and the identity mapping would map black positions to themselves (lit), defeating the feature. The fold path is required so black cells are dropped.

Fix: gate the identity fast path on "no black pixels." Cleanest is for `Layouts` to expose `hasBlackPixels()` (or the Layer to detect it during the dimension walk), and require it false for the identity path. When black pixels exist, route to `buildFoldedLUT`, which drops the black cells. **Regression pin (load-bearing):** a grid with NO black pixels must still take the identity path, byte-identical to today (the CLAUDE.md dense-fast-path constraint).

## Revert the abandoned driver-gap work (discrete first step)

Remove the `g`/`+` gap syntax entirely, as its own commit/step so the diff reads as "remove abandoned approach":
- **[PinList.h](src/light/drivers/PinList.h):** remove `kMaxGapsPerOutput`, `struct GapRuns`, `parsePinSegments`, the `GapRuns* gaps` out-param on `assignCounts` (and its fill loop), and the gap prose in the header comment. `assignCounts` returns to the plain number/list/broadcast parser.
- **[ParallelLedDriver.h](src/light/drivers/ParallelLedDriver.h):** remove `laneGaps_`, the gap semantics of `laneWire_`, the gap branch of `laneRowLit`, `laneNextBoundary`'s gap logic, the `laneGapCount`/`laneRowLitForTest`/`laneWire` test accessors, the `assignCounts(..., laneGaps_)` argument, the wire-length gap accumulation, and the `if (laneGaps_[i].n) return false` in `uniformLaneCounts`. Restore `laneRowLit` to the pre-gap `row < laneCounts_` test.
- **[RmtLedDriver.h](src/light/drivers/RmtLedDriver.h) / [NetworkSendDriver.h](src/light/drivers/NetworkSendDriver.h):** confirm the `assignCounts` call sites compile after the trailing param drops.
- **Tests:** remove the gap TEST_CASEs from [unit_RmtLedDriver_pins.cpp](test/unit/light/unit_RmtLedDriver_pins.cpp) and [unit_MultiPinLedDriver.cpp](test/unit/light/unit_MultiPinLedDriver.cpp).
- **Docs:** revert the `ledsPerPin` gap paragraph in [drivers.md](../../../moonmodules/light/drivers.md) and the backlog item edit in [backlog-light.md](../../../backlog/backlog-light.md).

## Tests (pin behavior)

- **`unit_GridLayout_blackpixel.cpp` (new):** `lightCount()` (physical) == full grid incl. black; `forEachCoord` emits black cells with `black==true` at their true (x,y) and physical `idx` advancing monotonically over ALL cells; robustness (all-black row, black-only grid, black range beyond width clamps, empty range byte-identical to no-black, 0x0x0); multi-child stitching (a black-bearing grid then a plain grid - the plain grid's physical indices start after the first grid's FULL physical count).
- **`unit_Layer_blackpixel_lut.cpp` (new):** with black pixels, `rebuildLUT` does NOT take the identity path (`lut().hasLUT()` true) and no LUT destination equals a black physical index; lit cells map to the correct physical position. **Regression:** a no-black grid still takes the identity path (`!lut().hasLUT()`), byte-identical.
- **`scenario_GridLayout_blackpixel.json` (new):** mirror `scenario_GridLayout_resize.json`; build Layouts to Layer(SolidEffect fill) to Drivers on a 16x16 grid, set the black range live (no reboot), assert the output buffer is black at black physical indices and the fill color at lit ones; clear the range back and confirm return to the dense identity path (liveness both directions). SolidEffect is the ideal probe - every lit pixel is a known non-black color, so a black slot is unambiguously a black pixel.

## Verification

1. **Desktop:** `cmake --build build --target mm_tests` then `ctest`; run `uv run moondeck/scenario/run_scenario.py`. New tests green; the no-black regression tests confirm the identity path unchanged. Full suite green (the `CoordCallback` signature change compiled through every lambda with no drift on existing scenarios).
2. **Spec-check + platform-boundary:** green (all `src/light/`, no platform code).
3. **On the S3 bench (192.168.1.158):** flash; add GridLayout + Layer(**SolidEffect**, solid red) + a driver; set the black range live and confirm (a) lit LEDs light red, (b) black-region LEDs stay dark on the wire while data reaches the lit LEDs beyond, (c) the **web preview shows the holes in place** at the black coordinates, (d) status/summary shows physical count > lit destinations. Toggle the range back to empty and confirm the whole strand lights (live, no reboot) - the identity fast path re-engages. Confirm FPS/heap unchanged for the no-black config. **Invite the PO to look; their eyes on the panel + preview are the measurement.**
