# MoonLive scripts

Scripts for the [MoonLive](../docs/moonmodules/light/MoonLiveEffect.md) engine, one file per script,
grouped by the module that runs it. Name one in a module's `script` control on a running device and
it compiles to native code on the next tick.

Each script declares a **class**, and the host calls its functions: `tick()` for an effect,
`placeLights()` for a layout, `modifyLogical()` for a modifier. A function is called when it is
present and its moment arrives, so which entry points a class defines is what decides what it does.
The class name is independent of the file name, the way a C file and the functions in it are.

A class may also define functions of its own and **call them**, including calling itself:

```
class CrosshairEffect {
  byte bpm = 30;

  void defineControls() { addControl("bpm", bpm, 1, 240); }

  void column(int cx) { for (int y = 0; y < height; y = y + 1) { setRGB(y * width + cx, 255, 40, 0); } }
  void tick()         { fill(0, 0, 0); column(scale(beat(bpm, t), width)); }
}
```

These are real calls, not pasted-in text: the callee gets its own frame when it runs, which is what
lets one helper call another and lets a function recurse. Arguments are passed BY VALUE, so a
function that writes a parameter changes its own copy and the caller's variable is untouched.
`effects/crosshair.mle` is the worked example; `layouts/sixteen-rings.mll` is the one where the
arguments earn their place, calling one helper sixteen times with different coordinates.

**A script is C++, and a compiler checks that.** Every shipped script compiles under
`c++ -std=c++20 -fsyntax-only` (`test/python/test_scripts_are_cpp.py`), so the language cannot drift
into a dialect one feature at a time: an editor highlights a script correctly, a reader brings their
C++ intuition, and a script stands a good chance in any engine that speaks the same subset.

One shape difference is deliberate, and the test bridges exactly that one: a class here needs no
`public:` and no trailing semicolon.

Anything else a compiler rejects is a divergence, and the test is where it surfaces.

**Every function declares what it returns**, the way the compiled module a script stands in for
does: `void tick()` beside `void tick() override`. Three types, which is all the language has values
for:

| Type | Means | Example |
|---|---|---|
| `void` | it acts, it answers nothing | `void tick() { … }` |
| `int` | a number: any whole value, and a `fixed` one | `int dimensions() { return 2; }` |
| `string` | a literal | `string tags() { return "🌀"; }` |

`return` leaves a function, with a value or without one. Inside `tick()` a bare `return;` is an
early exit, which is what a guard wants:

```
void tick() {
  if (width < 2) { return; }        // nothing to draw on a single column
  fill(0, 0, 0);
}
```

`string` names what comes back rather than introducing a string type: a script returns a literal,
and building, joining or comparing text is out of scope.

**A declaration is a MEMBER; `defineControls()` decides what the UI shows.** `byte bpm = 30;` is
state the script owns: visible in every function, surviving every tick. Naming it in
`defineControls()` with `addControl("bpm", bpm, 1, 240)` also puts it on the UI as a slider, which is
the same call a compiled module makes. A member no `addControl` names stays private to the script,
which is how a stateful effect holds a value the user should not see.

The default comes from the declaration, the range from the call, and the quoted name is the UI
label, free to differ from the member's name.

**A member can be WRITTEN, which is what makes it state.** `level = level + 10;` assigns, and the
value is still there on the next tick, because a member lives in storage that outlives the call. A
loop variable can be assigned too. A [system variable](../docs/moonmodules/light/MoonLiveEffect.md)
(`width`, `t`, `xPos`) cannot: the engine rewrites it before every call, so the store would vanish.

A control CAN be assigned, and the effect is visible rather than surprising: the value moves under
the slider until the user drags it again. Whether a member is a control is decided by
`defineControls()` at run time, so the language does not distinguish the two here.

**`if` and `else`,** with `<`, `<=`, `>`, `>=`, `==` and `!=`. Both sides are ordinary expressions:

```c
if (heat[i] > 40) { setRGB(i, 255, 90, 0); }
else { setRGB(i, 0, 0, 0); }
```

**Members can be wider than a byte, and can be arrays.** `byte` spans 0..255; `int` spans
-2,147,483,648..2,147,483,647, which is what a position on a wall wider than 255 needs. An array is
declared with a literal length and starts at zero:

```c
int phase = 900;      // a value a byte cannot hold
byte  heat[16];         // sixteen elements, all zero to begin with
```

**Every variable is declared, including a loop's counter.** A member states its type, an assignment
to a name that was never declared is refused, and a `for` writes `for (int i = 0; ...)`. One rule
with no exception, and the same line C++ would take.

An index is an arbitrary expression (`heat[i * 2 + 1]`), and an index outside the array is
**clamped to the last element** rather than refused or allowed through: a script computes indices
from live control values, so out of range is a normal run-time state, and the fixture shows a
repeated last light instead of crashing.

All of a class's members share a small fixed budget (`kCtrlBytes`), so a class that declares more
than fits is a compile error naming the arena, not a failed allocation while a fixture runs.

`effects/ember.mle` is the worked example: a heat array that decays and re-ignites, so what it
draws this frame depends on the last one. That is the line between an effect that evaluates a
formula and one that runs a simulation, and it is the reason arrays exist. `plasma.mle` would look
identical if every frame started from scratch; `ember.mle` would go dark.

**Declare a helper above the function that calls it.** Only functions already parsed are visible, so
a call to one declared further down reports `unknown function`. A function can always call itself.

**Recursion is bounded.** About 30 calls deep, a further call does nothing and returns. A render
task has a fixed stack, so the alternative to a limit is a device that resets mid-frame. What you
see if you hit it is the picture being wrong where the recursion stopped, on a device that keeps
running. Nothing is reported; the exact depth is `kMaxCallDepth`.

**A script says what it is: `dimensions()` and `tags()`.** Both optional, both named after the
member functions a compiled module declares (`Dim dimensions() const override`,
`const char* tags() const override`), and both read once when the script compiles.

```
class RainEffect {
  int dimensions() { return 2; }        // an x/y picture
  string tags() { return "✨"; }         // shown on the card and in the picker

  void tick() { fill(0, 0, 40); }
}
```

`dimensions()` returns 1, 2 or 3, and it decides how the layer EXTRUDES the script. A script that
returns 1 paints the x=0 column and the framework fans it across the width; one that returns 2
paints the z=0 slice and the framework copies it through the depth. So a script fills a rig it never
indexed, and a wrong answer is visible: declare 1 and paint a picture, and only the first column
survives. A script that stays silent is treated as 2, which is what every script rendered as before
this existed.

`tags()` returns the emoji shown beside the script, so a row in the picker reads like a compiled
effect's. The vocabulary is shared with the compiled modules: 📊 audio-reactive, ✨ particles,
🎯 aims moving heads. A script that declares none shows 📝, the mark of a scripted effect.

Both reach the picker before a factory script is downloaded, because the build extracts them from
the source into the catalog. That copy is for display only: once a script is on the device, the
compiled script is what decides.

**A script's ROLE is its file extension**: `.mle` an effect, `.mll` a layout, `.mlm` a modifier. One
language, three names, the way GLSL uses `.vert`/`.frag` for one shading language. It is what a card
filters its picker on, so an effect card offers effects.

Stated in the name rather than worked out from the file's contents, and deliberately: the entry
point a class defines (`tick`, `placeLights`, `modifyLogical`) already tells the ENGINE which moment
to call, but reusing that as the role would tie a UI filter to a language feature. The day a modifier
wants a per-frame `tick()`, every modifier would start appearing in effect pickers with nothing
changed. The engine stays role-blind either way: it runs whichever moment the binding asks for, so a
class defining several is still legal.

| folder | run by | a script writes |
|---|---|---|
| `layouts/` | [MoonLiveLayout](../docs/moonmodules/light/MoonLiveLayout.md) | where the lights physically are — `addLight(x, y, z)` |
| `effects/` | [MoonLiveEffect](../docs/moonmodules/light/MoonLiveEffect.md) | a color per light: `setRGB(index, r, g, b)`, or a whole shape at once with `line(x1, y1, x2, y2, r, g, b)` |
| `modifiers/` | [MoonLiveModifier](../docs/moonmodules/light/MoonLiveModifier.md) | where one light lands: `setXYZ(xPos, yPos, zPos)` |

Each module ships one of these as its default, so the folder doubles as the reference for what a
working script looks like.

`unit_MoonLiveScripts` compiles every file here, so a script that stops parsing when the language
changes fails the build rather than waiting to be pasted into a device.

## Editing a shipped script forks it

A device keeps two copies of the library. The ones that ship live in `/.moonlive`; anything you edit
on the device is saved to `/moonlive`, and **your copy is the one that runs**. That is what makes an
edit reversible: the original is still there, so the card's delete button becomes a **revert arrow**
(↺) for a script you have changed, and pressing it brings the shipped version back without needing a
network.

The cost of that arrangement is that your copy also HIDES later updates to the shipped one, so the
card says which case it is in. Its status carries one of:

| status | what it means |
|---|---|
| (nothing) | the shipped script is running, unedited |
| `edited copy` | your version is running; the shipped one has not changed since you forked it |
| `edited copy, shipped one updated` | your version is running, and **the library has a newer one** |

The third is the one to act on: revert to take the new version (losing your changes), or keep yours
and ignore it. Nothing is decided for you, and nothing overwrites an edit.

Two details worth knowing. Opening a shipped script and saving it **without changing anything**
creates no copy, so browsing the library cannot accidentally pin a script at today's version. And
the comparison is against the version you actually forked from, recorded when the fork is made, so
editing your own copy later does not make an outdated fork look current.

## Debugging: print

`print(v)` writes a value to the serial log and returns it, so it wraps any part of an expression
without changing the result — `addLight(print(xx), yy, 0)` places the same light and tells you what
`xx` was.

**Take it out again when the script works.** A serial write blocks, and a script runs on the render
tick, so a print costs frame time every frame it survives. Each compile grants a short burst and then
goes quiet, which bounds the damage and gives every edit a fresh window; it does not make a print
free. No script in this folder ships with one.
