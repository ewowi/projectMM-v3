# MoonLiveModifier

A **modifier written as a live script**: the coordinate transform that decides where each light sits in the pattern, authored as text on a running device instead of compiled in as a C++ class. Same [MoonLive](MoonLiveEffect.md) engine as a scripted effect, pointed at a different job.

A [modifier](modifiers.md) reshapes how a Layer's output maps onto the physical lights — mirror it, shift it, swap its axes. Each hand-written one is a class, a rebuild and a reflash. A scripted one is a line of text, applied as you type.

<img src="../../assets/light/MoonLiveModifier.png" width="300" alt="MoonLiveModifier">

## Writing one

The script transforms **one coordinate**. It needs no loop over the lights, because the Layer already does that: it calls the script once per physical light while it builds its mapping. (A `for` is available if the arithmetic wants one — it just is not how the script reaches the next light.)

```c
class MirrorModifier {
  void modifyLogical() { setXYZ(width - 1 - xPos, yPos, zPos); }   // mirror along x
}
```

The function is named `modifyLogical` because that is the moment a modifier is asked about: the Layer calls it once per light while building its mapping, and a script that does not define it passes every light through unchanged. An effect's moment is `tick`, a layout's is `placeLights`.

The body is one expression per axis. Other shapes, in the same place:

```c
setXYZ(yPos, xPos, zPos);               // swap the axes
setXYZ(xPos + 4, yPos, zPos);           // shift by four
setXYZ((width - 1 - xPos) * 2, yPos, zPos);   // mirror, then stretch
```

`setXYZ(x, y, z)` writes the transformed position, mirroring `setRGB(index, r, g, b)`. The index is the destination slot: today the script is handed a single coordinate, so it is always `0`.

### What a script can read

`x`, `y`, `z` (the light being folded) and `width`, `height`, `depth` (the box it lives in) are [system variables](MoonLiveEffect.md#system-variables-what-the-engine-hands-a-script) — the engine writes them per call, and a script cannot declare a name that shadows one.

`width` matters more than it looks. A mirror written against a fixed `255` sends every light of a 16-wide grid far outside the grid, the Layer discards each one as out of bounds, and the fixture goes black — with no error anywhere, because the script itself ran perfectly.

### Seeing inside a script

`print(v)` logs a value and returns it, so it wraps any part of an expression: `setXYZ(print(width - 1 - xPos), yPos, zPos)`.
It is for debugging and comes back out again: [what print costs](https://github.com/MoonModules/projectMM/blob/main/moonlive/README.md#debugging-print).

## Limits

**A coordinate is a byte, so an axis spans 0..255.** A position handed TO a script outside that range is passed through untransformed rather than wrapped. A position a script COMPUTES past 255 keeps its low byte, so `(width - 1 - x) * 2` on a grid wider than 128 lands somewhere unintended, so keep a computed result inside the box. A script's own MEMBERS may be `int`, so intermediate arithmetic can exceed 255 even where the coordinate handed back cannot.

**A script cannot resize the logical box.** A modifier has two hooks: one reshapes the box once per rebuild, one folds each coordinate. A script drives only the second, so transforms that keep the box the same size work, and ones that halve it (the way the built-in [Mirror](modifiers.md#mirror) does) need the compiled modifier.

**The grammar is arithmetic over calls**: `+`, `-`, `*`, parentheses, the usual precedence, `for`, and `if` with the six comparisons. Division and `%` are not operators; `mod(a, b)` and `turn(n)` are the calls that cover them.

## What the card tells you

`status` is the size of the compiled program; the memory figure is what the module costs the device
(its own `sizeof`, plus the exec block and control arena); `tickTimeUs` is the real per-tick cost.
Past half full, the status also names the tightest limit the script is approaching. Detail:
[MoonLive](MoonLiveEffect.md#what-the-card-tells-you-size-memory-and-how-close-to-a-wall).

## Controls

| control | what it does |
|---|---|
| `script` | the script's file name, picked from the [library](MoonLiveEffect.md) or your own; naming it (or re-naming it after an edit) recompiles and re-maps live |

Plus one control per `addControl` in the script's `defineControls()`: `addControl("amount", amount, 0, 64)`
becomes a slider, and moving it rebuilds the mapping just as editing the script does.

Editing the script asks the Layer to rebuild its mapping, so a change is visible immediately. A script that fails to compile shows the parse error on the module and the mapping falls back to passing coordinates straight through — the transform disappears until the script parses again, and the device keeps rendering throughout.

Detail: [technical](moxygen/MoonLiveModifier.md)
