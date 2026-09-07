# Plan — Stage 3 E_MoonModules batch (GameOfLife + GEQ3D + PaintBrush)

The first effect-migration batch of [MoonLight migration Stage 3](../Plan-20260630%20-%20MoonLight%20migration%20%28multi-stage%29.md): port all three effects in MoonLight's `Nodes/Effects/E_MoonModules.h` (MoonModules-authored set), built fresh on the Stage-1 primitives (palette, `math8`, `noise`, `draw`). One commit (PO decision). Each effect: study behaviour → reimplement against EffectBase → unit + scenario test → `effects.md` row.

**Method (CLAUDE.md):** study the MoonLight source for *behaviour* (controls, algorithm, state), then write our own code on our architecture — never trace/copy. FastLED/MoonLight credited as prior art in each `tags()` + the effect's header + the `effects.md` row.

## Decisions locked (product owner, 2026-06-30)

- **All three in one commit** (the E_MoonModules batch).
- **GameOfLife: FULL faithful port** — all 7 rulesets + custom `B#/S#` parser, CRC16 stasis/oscillator/spaceship detection, infinite-mode pentomino/glider respawn, color-by-age, blur, 2D **and** 3D (8- vs 26-neighbour). The Stage-1 "proof effect" done properly, on the new palette + `random8`.
- **Skip soft/anti-aliased lines** — our `draw::line` is hard-edged; drop the `soft` control on GEQ3D/PaintBrush. Crisp lines read fine on an LED grid; an AA `draw::line` is a later foundation add if visibly needed.

## The three effects

### GameOfLifeEffect (2D/3D, 💫🌙 MoonModules)
- **Controls:** `backgroundColor` (Coord3D/RGB), `ruleset` (select: Custom/Conway B3-S23/HighLife/InverseLife/Maze/Mazecentric/DrighLife), `customRule` (text "B#/S#"), `speed` (0-100, gen/s), `density` (10-90% initial life), `mutation` (0-100%), `wrap` (bool), `colorByAge` (bool: green→red), `infinite` (bool: respawn), `blur` (0-255 dead-cell fade). (`disablePause` dropped — UI nicety, not behaviour.)
- **State (heap, onBuildState — like Fire's heat_/Wave's trail_):** `cells_`/`future_` bit-packed (count/8 bytes each), `colors_` palette-index per cell (count bytes), `generation_`, `step_` ms, parsed `birth_[9]`/`survive_[9]` bool arrays, CRC history (`oscCrc_`/`shipCrc_`) for stasis detection. ~20 KB at 128² → PSRAM via `platform::alloc`, freed in teardown + dtor. NOT inline members (the HueDriver stack lesson).
- **Algorithm:** gen 0 = random fill by `density`, random palette color per live cell. Each gen (throttled by `speed`): count neighbours (8 in 2D, 26 in 3D, `wrap` toroidal), apply `birth_/survive_`, newborns inherit a neighbour's color with `mutation` chance of a fresh one; dead cells fade toward `backgroundColor` by `blur`. `colorByAge` overrides color (green new → red aging). After each gen, CRC16 the grid; on detected oscillation/extinction, `infinite` ? place an R-pentomino/glider : reset to gen 0.
- **New primitive needed:** `crc16` (textbook CCITT) — add to a small `core/crc.h` (a hash, not math8; reusable, ~15 lines). Uses `Random8` (math8) for fill/mutation/respawn placement, `colorFromPalette` for color, `fadeToBlackBy`/`blend` for the dead-cell blur.

### GEQ3DEffect (2D, audio ♫, 💫🌙)
- **Controls:** `speed` (1-10 projector sweep), `frontFill` (0-255), `horizon` (0..width-1 vanishing row), `depth` (0-255 perspective), `numBands` (2-16), `borders` (bool). (`softHack` dropped.)
- **State:** `projector_` x-position + `projectorDir_` (±1), `counter_` frame throttle.
- **Algorithm:** sweep the projector (vanishing point) left/right at `speed`, bounce at edges. Map `AudioFrame::bands[]` → bar heights (75-85% of height); scale band indices if `numBands` < 16. Per band draw a 3D-perspective bar: darker side edges, a top surface with perspective lines toward the projector, a front fill (`frontFill` blend) bottom→height, optional `borders`. Bands left of the projector paint right-to-left, right side left-to-right. Reads `AudioModule::latestFrame()` (the AudioSpectrum pattern); uses `draw::line` + `blend`.

### PaintBrushEffect (3D, audio ♫, 💫🌙)
- **Controls:** `oscillatorOffset` (0-16 phase mult), `numLines` (2-255), `fadeRate` (0-128 bg decay), `minLength` (0-255 draw threshold), `colorChaos` (bool per-line hue), `phaseChaos` (bool per-frame jitter). (`soft` dropped.)
- **State:** `hue_` (cycles per frame), `chaos_` (per-frame random phase or 0).
- **Algorithm:** each frame: advance `hue_`, fade the whole field by `fadeRate` (`fadeToBlackBy` over the buffer). Per line (0..numLines): map line→audio band; build two 3D endpoints via `beatsin8(... oscillatorOffset, ms)` modulated by band amplitude; Euclidean distance × band magnitude = length; if length > `minLength` draw `draw::line(a, b, color)`. Color = `colorChaos` ? per-line hue+`hue_` : per-band gradient. Uses `beatsin8` (math8), `AudioFrame::bands[]`, `draw::line`, `fadeToBlackBy`.

## Files

- **New:** `src/light/effects/GameOfLifeEffect.h`, `GEQ3DEffect.h`, `PaintBrushEffect.h`; `src/core/crc.h` (crc16 for GoL stasis).
- **Edit:** `src/main.cpp` (register the 3, each → `light/effects.md#<anchor>`), `test/scenario_runner.cpp` (register for scenarios), `docs/moonmodules/light/effects.md` (3 rows + `## Source` links + anchors), `test/CMakeLists.txt` (the new unit tests).
- **Tests (new):** `unit_GameOfLifeEffect.cpp` (Conway still-life stays, blinker oscillates, B/S parser, neighbour count 2D+3D, no-crash on 0×0×0), `unit_crc.cpp` (crc16 known vectors), and GEQ3D/PaintBrush covered by the shared render test (`unit_effects_render` STATELESS / non-zero with a fed AudioFrame) + a scenario. Audio effects render dark on silence (safe) — assert structure on a synthetic frame where testable.
- **Scenarios:** add the 3 to the perf/all-effects sweep; GoL gets its own scenario (gen progression renders + doesn't crash at any grid size).

## Hard-rule conformance

- **Hot path:** integer-only; GoL's grid state is heap (PSRAM), allocated off the hot path in `onBuildState`, freed in teardown/dtor — never inline (sizeof stays small; the registerType-probe stack stays tiny, per the HueDriver/P4 lesson). The neighbour sweep is integer; the per-frame render writes the buffer directly.
- **Effects at every grid size:** all three guard 0×0×0 and tiny grids (GoL with <1 cell = no-op; GEQ3D/PaintBrush clip via `draw::`). Animation math doesn't truncate to zero on a fast tick.
- **Audio safety:** GEQ3D/PaintBrush read `latestFrame()`; no mic → zero bands → dark, safe on any target (the AudioSpectrum/Volume contract).
- **Domain/platform:** pure light-domain effects on EffectBase; no `#ifdef`, no platform calls (heap via `platform::alloc`).

## Verification

- Build -Werror (desktop + ESP32). `ctest` green incl. the new GoL + crc tests. Scenarios green incl. the new GoL scenario + the 3 in the sweep. `check_specs.py` green (3 new control sets all named in `effects.md`).
- KPI: GoL is the heaviest (full-grid neighbour sweep) — measure its tick; it throttles by `speed` so the per-frame cost is the render, not the update, most frames.
- Bench on P4 + S3: each effect renders; GoL progresses generations + respawns; the two audio effects react to sound. (Given the stack lesson, confirm GoL's heap state doesn't bloat sizeof — it's pointers, not arrays.)

## Out of scope

- `disablePause` / `softHack` / `soft` controls (UI niceties / AA we don't do yet).
- Anti-aliased `draw::line` (a later foundation add).
- The other `E_*.h` files (E_MoonLight, E_WLED) — later Stage 3 batches.
