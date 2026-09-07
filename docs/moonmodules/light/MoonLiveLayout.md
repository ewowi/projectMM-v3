# MoonLiveLayout

A **layout written as a live script**: where the lights physically are, authored as text on a running device instead of compiled in as a C++ class. Same [MoonLive](MoonLiveEffect.md) engine as a scripted effect or [modifier](MoonLiveModifier.md), pointed at the third job.

A [layout](layouts.md) is the one part of the pipeline that differs for every physical build — a ring, a spiral staircase, a car grille, a costume sewn last night. Each one has meant writing a C++ class, rebuilding and reflashing. A script means the person who hung the lights can describe where they went, on the device, and see it immediately.

<img src="../../assets/light/MoonLiveLayout.png" width="300" alt="MoonLiveLayout">

## Writing one

The script places every light itself, with a loop. That is the difference from a scripted modifier: the Layer calls a modifier once per light, so its script transforms a single coordinate — a layout has no such per-light call to ride on.

```c
class GridLayout {
  byte cols = 16;
  byte rows = 16;

  void defineControls() {
    addControl("cols", cols, 1, 64);
    addControl("rows", rows, 1, 64);
  }

  void placeLights() {
    for (int y = 0; y < rows; y = y + 1) {
      for (int x = 0; x < cols; x = x + 1) {
        addLight(x, y, 0);
      }
    }
  }
}
```

That is the default: a plain grid, one light per cell. The function is named `placeLights` because that is the moment a layout is asked about: the module calls it when the fixture is being built, and a script that does not define it places nothing. An effect's moment is `tick`, a modifier's is `modifyLogical`, and a class may define any of them. `addLight(x, y, z)` places the next light along the strand — no index, because the order the script calls it in *is* the strand order.

The `cols` and `rows` lines are the script's own controls, not something the module hands it. A layout is never told how big it is: the pipeline works out the bounding box from the coordinates the layouts actually place, so a size passed in from outside would be a second answer that could disagree with the first.

They are named `cols`/`rows` because `width`, `height` and `depth` are [system variables](MoonLiveEffect.md#system-variables-what-the-engine-hands-a-script) — the logical grid the Layer hands an effect or a modifier. A layout is upstream of that grid, so it names its own controls.

A few shapes that are one line here and a new class otherwise:

```c
// a strand that runs right to left
for (int i = 0; i < cols; i = i + 1) { addLight(cols - 1 - i, 0, 0); }

// a diagonal
for (int i = 0; i < cols; i = i + 1) { addLight(i, i, 0); }

// two rows, stacked
for (int i = 0; i < cols; i = i + 1) { addLight(i, 0, 0); addLight(i, 1, 0); }

// a circle: lights and grid cells are not the same number
// (`count` and `radius` are members, surfaced by addControl in defineControls)
for (int i = 0; i < count; i = i + 1) {
  addLight(scale(cos(i * turn(count)), radius * 2 + 1),
           scale(sin(i * turn(count)), radius * 2 + 1), 0);
}
```

### What a script can read

A script reads whatever it declares. `byte cols = 16;` is a member the script owns; naming it in `defineControls()` with `addControl("cols", cols, 1, 64)` also makes it a real slider in the UI, and the loop reads it, which is how a panel gets resized without editing code. A member whose value a byte cannot hold is declared `int` and surfaced by the same call — the widget follows the type, so the two cannot disagree. A member no such call names stays private to the script.

`t` is the one [system variable](MoonLiveEffect.md#system-variables-what-the-engine-hands-a-script) a layout is given, and it is always **0** here: the script runs twice per rebuild (once to count, once to place) and must agree with itself, so it is handed a fixed clock rather than a live one — a moving `t` would let the two passes disagree on how many lights there are. `width`/`height`/`depth` name the grid a layout is *defining*, so asking for one is a compile error rather than a silent zero; `x` and `y` are free to use as loop counters.

### Seeing inside a script

`print(v)` logs a value and returns it, so it wraps any part of an expression: `addLight(print(x), y, 0)`.
It is for debugging and comes back out again: [what print costs](https://github.com/MoonModules/projectMM/blob/main/moonlive/README.md#debugging-print).

## How the count is known

A layout has to answer **how many lights** before it produces a single coordinate — the Layer sizes its buffer from that number and only then asks where each light is. A script cannot be asked "how many?" without running it.

So it runs twice. On the first pass `addLight` counts; on the second it emits each position to whoever asked. Same script, same arithmetic, so as long as the script is deterministic the two answers cannot drift apart — which is exactly what the compiled layouts do (`SphereLayout` walks its shell twice for the same reason). A script that calls `random16` breaks that condition. The two passes disagree on the COUNT only when the random value decides a loop bound or how many times `addLight` runs; a random COORDINATE keeps the count right and simply places the lights somewhere else on the second pass, so the fixture is the size it claims but not the shape. See [Limits](#limits).

**Nothing is stored between the passes.** Staging 16,384 coordinates would cost 48 KB, which a classic ESP32 driving that many lights does not have spare. Running the script again is cheaper than remembering what it said, and it means a scripted layout costs the same as a compiled one: the JIT'd program, and nothing that grows with the light count.

## Limits

**The grammar is arithmetic, calls, `for` and `if`**: `+`, `-`, `*`, parentheses, nested loops, and the six comparisons (`<`, `<=`, `>`, `>=`, `==`, `!=`). Division and `%` are not in the language, so where a script would divide it calls `mod(a, b)` or `turn(n)` instead.

A serpentine (every other row reversed) is what `if` makes expressible, and it is the common panel wiring:

```c
byte odd = 0;
for (int y = 0; y < rows; y = y + 1) {
  for (int x = 0; x < cols; x = x + 1) {
    if (odd == 0) { addLight(x, y, 0); }
    else { addLight(cols - 1 - x, y, 0); }
  }
  if (odd == 0) { odd = 1; } else { odd = 0; }
}
```

**A script runs twice per rebuild**, once to count and once to place, so it has to be deterministic. With `random16` in a loop bound or around an `addLight` call, the two passes disagree on the count; with `random16` in a coordinate, the count holds and only the positions move.

## What the card tells you

`status` is the size of the compiled program; the memory figure is what the module costs the device
(its own `sizeof`, plus the exec block and control arena); `tickTimeUs` is the real per-tick cost.
Past half full, the status also names the tightest limit the script is approaching. Detail:
[MoonLive](MoonLiveEffect.md#what-the-card-tells-you-size-memory-and-how-close-to-a-wall).

## Controls

| control | what it does |
|---|---|
| `script` | the script's file name, picked from the [library](MoonLiveEffect.md) or your own; naming it (or re-naming it after an edit) recompiles and re-places the lights live |

Plus one control per `addControl` in the script's `defineControls()`.

Editing any of them rebuilds the pipeline, because every one can change where the lights are. A script that fails to compile leaves a fixture with no lights, shows the parse error on the module, and the device keeps running.

Detail: [technical](moxygen/MoonLiveLayout.md)
