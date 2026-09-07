# Layouts

![layouts controls](../../assets/core/Layouts.png)

Every layout, one block each: what it does and what each control means — together. A layout maps light indices to physical `(x, y, z)` positions — it defines the *shape* an [effect](effects.md) draws onto and a [driver](drivers.md) sends out. The [Layouts](moxygen/Layouts.md) container holds one or more layout children and composes them into one coordinate space; a [Layer](moxygen/Layer.md) renders over that combined space. (For how this page maps to the source/asset folders, see the [folder-structure decision](../../adr/0015-library-is-a-tag-not-a-folder.md).)

## MoonLight layouts

<a id="carlights"></a>

### Car Lights

A pair of concentric-ring "headlight" clusters (nested rings of 1/8/12/16/24 LEDs) positioned to mimic a car's front lights — a fixed arrangement composed from [Ring](#ring) geometry.

<img src="../../assets/light/layouts/CarLightsLayout.gif" width="300" alt="Car Lights layout preview">

- `scale` — overall size scale (1–10).

Origin: Eric Marciniak (Discord) — custom car-lights fixture

Detail: [technical](moxygen/CarLightsLayout.md)

<a id="cube"></a>

### Cube

A 3D cube volume, `width`×`height`×`depth`, wired in a configurable axis order with optional per-axis serpentine — the 3D generalisation of Panel.

- `width` / `height` / `depth` — cube extent per axis (1–128).
- `wiringOrder` — the axis nesting order the strip follows.
- `X++` / `Y++` / `Z++` — count up (vs down) along that axis.
- `snakeX` / `snakeY` / `snakeZ` — serpentine (alternate rows/columns reverse) on that axis.

Origin: MoonLight · via [MoonLight](https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Nodes/Layouts/L_MoonLight.h)

Detail: [technical](moxygen/CubeLayout.md)

<a id="humansizedcube"></a>

### Human-Sized Cube

A hollow walk-in cube built from five LED-curtain faces (front, back, top, left, right), each a `width`×`height`×`depth` curtain — for large/room-scale cube installations.

- `width` / `height` / `depth` — cube extent per axis (1–20).

Origin: MoonLight · via [MoonLight](https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Nodes/Layouts/L_MoonLight.h)

Detail: [technical](moxygen/HumanSizedCubeLayout.md)

<a id="panel"></a>

### Panel

A 2D matrix panel with full wiring control: choose the axis order, per-axis direction, and serpentine — the general matrix layout ([Grid](#grid) is the simple case).

- `panelWidth` / `panelHeight` — panel size in lights (1–512).
- `wiringOrder` — `XY` (rows) or `YX` (columns) nesting.
- `X++` / `Y++` — count up vs down along that axis.
- `snake` — serpentine wiring (alternate lines reverse).

Origin: MoonLight · via [MoonLight](https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Nodes/Layouts/L_MoonLight.h)

Detail: [technical](moxygen/PanelLayout.md)

<a id="panels"></a>

### Panels

Tiles an M×N grid of full matrix panels into one large display: an outer walk over the panel grid plus an inner walk over each panel's lights, both independently wired — for multi-panel video walls.

- `horizontalPanels` / `verticalPanels` — panel-grid size (1–32 each).
- `wiringOrderP` / `X++P` / `Y++P` / `snakeP` — the panel-to-panel wiring (order, direction, serpentine).
- `panelWidth` / `panelHeight` — each panel's size (1–512).
- `wiringOrder` / `X++` / `Y++` / `snake` — the per-panel light wiring.

Origin: MoonLight · via [MoonLight](https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Nodes/Layouts/L_MoonLight.h)

Detail: [technical](moxygen/PanelsLayout.md)

<a id="ring"></a>

### Ring

A single ring of LEDs evenly spaced around a circle — `nrOfLEDs` points, starting at `angleFirst`, spanning `rotation` degrees.

- `nrOfLEDs` — LEDs around the ring (1–255).
- `angleFirst` — starting angle in degrees.
- `rotation` — arc spanned (360 = full circle).
- `clockwise` — direction of travel.
- `scale` — spacing/radius scale.

Origin: MoonLight · via [MoonLight](https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Nodes/Layouts/L_MoonLight.h)

Detail: [technical](moxygen/RingLayout.md)

<a id="rings241"></a>

### Rings 241

The classic 241-LED concentric-ring disc: nested rings of 1, 8, 12, 16, 24, 32, 40, 48, 60 LEDs sharing a center.

- `scale` — overall radius scale (1–10).
- `outside in`: light 0 on the outer ring, wired inward, instead of at the center wired outward. The direction around each ring is unchanged.
- `angleFirst`: where light 0 of each ring sits, in degrees from the bottom (0-359), the same control [Ring](#ring) has. A disc is soldered with its first LED wherever the builder started, so this turns the image to match the hardware rather than re-wiring it. 0 is the unrotated placement.

Origin: MoonLight · via [MoonLight](https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Nodes/Layouts/L_MoonLight.h)

Detail: [technical](moxygen/Rings241Layout.md)

<a id="singlecolumn"></a>

### Single Column

A vertical line of LEDs at a fixed X — the 1D column primitive.

- `starting Y` — the column's start row.
- `height` — LEDs in the column (1–1000).
- `X position` — the column's x.
- `reversed order` — wire top-to-bottom instead of bottom-to-top.

Origin: MoonLight · via [MoonLight](https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Nodes/Layouts/L_MoonLight.h)

Detail: [technical](moxygen/SingleColumnLayout.md)

<a id="singlerow"></a>

### Single Row

A horizontal line of LEDs at a fixed Y — the 1D row primitive.

- `starting X` — the row's start column.
- `width` — LEDs in the row (1–1000).
- `Y position` — the row's y.
- `reversed order` — wire right-to-left instead of left-to-right.

Origin: MoonLight · via [MoonLight](https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Nodes/Layouts/L_MoonLight.h)

Detail: [technical](moxygen/SingleRowLayout.md)

<a id="spiral"></a>

### Spiral

A conical spiral: `ledCount` LEDs winding up a cone from `bottomRadius` to a point over `height`.

- `ledCount` — LEDs along the spiral (1–2048).
- `bottomRadius` — radius at the base.
- `height` — spiral height.

Origin: MoonLight · via [MoonLight](https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Nodes/Layouts/L_MoonLight.h)

Detail: [technical](moxygen/SpiralLayout.md)

<a id="torontobargourds"></a>

### Toronto Bar Gourds

Maps a set of decorative "gourd" objects (a specific bar installation), each rendered at one of three granularities — one light per gourd, per side, or per LED.

<img src="../../assets/light/layouts/TorontoBarGourdsLayout.gif" width="300" alt="Toronto Bar Gourds layout preview">

- `granularity` — `One Gourd One Light`, `One Side One Light`, or `One LED One Light`.
- `nrOfLightsPerGourd` — LEDs per gourd in the coarsest mode (1–128).

Origin: [troyhacks](https://github.com/troyhacks/WLED) — custom Toronto bar gourd installation

Detail: [technical](moxygen/TorontoBarGourdsLayout.md)

<a id="tubes"></a>

### Tubes

Parallel vertical tubes: `nrOfTubes` columns of `ledsPerTube` LEDs, spaced `tubeDistance` apart.

- `nrOfTubes` — number of tubes (1–64).
- `ledsPerTube` — LEDs per tube (1–255).
- `tubeDistance` — spacing between tubes.
- `reversed` — reverse the wiring order.

Origin: MoonLight · via [MoonLight](https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Nodes/Layouts/L_MoonLight.h)

Detail: [technical](moxygen/TubesLayout.md)

## projectMM-native layouts

<a id="grid"></a>

### Grid

A dense 3D grid, row-major (x fastest, then y, then z); every position maps to a light.

- `width` / `height` / `depth` — grid extent on each axis in lights (1–512).
- `serpentine` — boustrophedon-wire alternate rows (every other row runs in reverse, matching a snaked strip).

Origin: MoonLight · via [MoonLight](https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Nodes/Layouts/L_MoonLight.h)

Detail: [technical](moxygen/GridLayout.md)

[Tests](../../tests/unit-tests.md#gridlayout)

<a id="gridblacks"></a>

### GridBlacks

A [Grid](#grid) with **mid-strand dark columns (a spacer)**. Columns `[blackStart, blackStart+blackCount)` are held black in every row, for a sealed/continuous panel that must stay dark down a strip, or a slat wall. A dark column is still a physical wire position the driver clocks, so WS2812 data flows *through* the unlit LEDs to reach the lit columns beyond; the lit columns keep their true positions (the picture is holed, not squeezed), so an effect maps straight across the gap. Use plain [Grid](#grid) when you need no dark columns.

- `width` / `height` / `depth` — grid extent on each axis in lights (1–512).
- `serpentine` — boustrophedon-wire alternate rows.
- `blackCount` — number of dark columns; `0` (the default) means no gap, so it renders exactly like a Grid.
- `blackStart` — first dark column (shown only once `blackCount` is set).

Origin: projectMM

Detail: [technical](moxygen/GridBlacksLayout.md)

[Tests](../../tests/unit-tests.md#gridblackslayout)

<a id="sphere"></a>

### Sphere

Lights on the surface of a hollow sphere — a one-light-thick shell inside a `(2·radius+1)³` box, no interior lights.

- `radius` — surface radius in light-units (1–64); the shell is every cell whose distance from the centre rounds to `radius`.

Origin: MoonLight · via [MoonLight](https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Nodes/Layouts/L_MoonLight.h)

Detail: [technical](moxygen/SphereLayout.md)

[Tests](../../tests/unit-tests.md#spherelayout)

<a id="wheel"></a>

### Wheel

A bicycle-wheel: `spokes` straight rows radiate from a centre hub, each carrying `ledsPerSpoke` LEDs spaced one unit apart outward.

- `spokes` — number of spokes radiating from the hub (2–64).
- `ledsPerSpoke` — LEDs along each spoke, spaced one unit apart from the centre outward.

Origin: MoonLight · via [MoonLight](https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Nodes/Layouts/L_MoonLight.h)

Detail: [technical](moxygen/WheelLayout.md)

[Tests](../../tests/unit-tests.md#wheellayout)

The [Layouts](moxygen/Layouts.md) container itself takes no controls — see its page for coordinate iteration, reordering, and rebuild propagation.

