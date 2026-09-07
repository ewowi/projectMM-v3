# Plan: a MoonLive script declares `dimensions()` and `tags()`

## Context

A compiled effect declares what it is: `Dim dimensions()` drives Layer extrusion, `const char*
tags()` gives the picker its emoji. A script declares neither. Every script is `MoonLiveEffect`,
which hardcodes `Dim::D2` and `tags() { return "📝"; }`, so every script extrudes as 2D and shows
the same notepad.

The PO's decision: the script declares both, as functions, because the language should read as a
real subset of C++ and be consistent with the entry points it already has.

**Why the script and not the catalog.** A user-written script never appears in the catalog: the
catalog is generated at build time from `moonlive/` in the repo. If dim and tags lived only there,
a custom script could never declare them, and would get the wrong extrusion with no way to fix it.
The script is the only place the information can live. The catalog becomes a build-time *extract*
of what the scripts already declare, never the source of truth.

This is already the engine's model: MoonLive.h:52 states that a script's ROLE is a question of what
it defined rather than what type it is, and `entry(name)` looks up any function by name. Dimensions
and tags are the same idea.

## The blocker: the language has no `return`

Checked before planning: there is no `return` keyword, no `Ret` IR op, and `CtrlFn` is

```c++
using CtrlFn = void (*)(uint8_t* buf, uint32_t nLights, uint8_t cpl, uint32_t t, const uint8_t* ctrls);
```

Every entry point today writes into a buffer and returns nothing. A script function that *answers*
a question is a new shape, so this is a language feature, not two new entry points. It is also a
feature worth having on its own: a helper that computes a value currently cannot hand it back.

## Design

### 1. `return <expr>;` in the language

- Lexer: `return` becomes a keyword.
- Compiler: a `Ret` IR op carrying an optional VReg; a bare `return;` in a void function.
- Emit: move the VReg into the platform's return register (a0 Xtensa, x10 RISC-V, x0 arm64,
  eax x86-64), then the existing epilogue. Each backend already emits an epilogue; this prefixes
  one move.
- ABI: `CtrlFn` stays `void`. A second alias `ValueFn` with the identical parameter list returning
  `uintptr_t` is what the host calls a value function through. Same block, same offsets, same
  arena: only the host's view of the return register differs. A `void`-returning function called
  as a value returns whatever is in the register, which is why the binding calls only functions
  the script actually declared as returning.
- Type checking stays as loose as the rest of the language: the value is a machine word.

**A `return` inside `tick()` is an early exit**, which is a real gain by itself and pins the
feature with a test that has nothing to do with dim or tags.

### 1b. Declared return types

Every function declares what it gives back, so the language reads as the C++ subset it claims to
be and the host stops relying on convention:

```
class MyEffect {
  int dimensions() { return 2; }
  string tags() { return "🌀"; }
  void tick() { ... }
}
```

Three types, which is all the language has values for: `void`, `int` (a machine word: every number,
including a Q16.16 fixed value, exactly as members already work) and `string` (a literal's pointer).

**A script should read like the compiled module it stands in for.** That is what picks these names
and this shape: `void tick()`, `int dimensions()`, `string tags()` sit beside
`void tick() override`, `Dim dimensions() const override`, `const char* tags() const override`. The
types differ only where the language genuinely cannot spell the C++ one (`Dim`, `const char*`), and
`const`/`override` are absent because a script has neither concept.

**Why this is a fix and not decoration.** `runValue()` calls through `ValueFn` whatever the script
declared. Call a `void` function that way and the host reads whatever sat in the return register: a
plausible garbage number. Today's guard is "only call what the script defined", which is a
convention. A declared type makes it checkable: the compiler records the type per entry, the
binding refuses to read a value from a `void` function, and a `string tags()` that returns nothing
is a compile error rather than a silent empty emoji. It also gives the catalog generator (step 4) a
far stronger key than a bare name.

**Migration.** A bare name is NOT accepted as implicit `void`: half-typed is the worst of both, and
the sweep test compiles every shipped script so nothing slips through silently. All 34 shipped
scripts, the docs and the test scripts gain their types in this step.

**`string` is honest about its limit.** The language has string LITERALS, not a string type: a
script can return one, not build, concatenate or compare one. `string` names what comes back; a
declaration is refused anywhere else, so the limit is enforced rather than discovered.

### 2. `dimensions()` and `tags()` as script functions

```
class MyEffect {
  int dimensions() { return 2; }
  string tags() { return "🌀"; }
  void tick() { ... }
}
```

- **Named `dimensions()`, matching the 58 compiled modules that declare
  `Dim dimensions() const override`**, because a script should read as much like a compiled module
  as it can. The RETURN TYPE is the one place they cannot match: a script has no way to name `Dim`,
  so it returns a plain 1/2/3. That costs nothing, because the enum IS those numbers
  (`D1 = 1, D2 = 2, D3 = 3`) and core already reduces the compiled probe's result to a byte
  (ModuleFactory.h:31) precisely so the light-domain enum stays out of core. The binding converts.
- Out of range or absent → D2, today's behavior, so every existing script keeps working unchanged.
- `tags()` mirrors the compiled `const char* tags() const override` exactly, down to the name, and
  returns a string literal. String literals already compile to a pointer into the source
  (MoonLiveCompiler.cpp:684), so this needs no new mechanism; the host reads it as `const char*`.
  The **source text must outlive the call**, which it does: the script text is held for the life of
  the compiled program. Pin that with a test that reads tags after a re-render.
- Both are called ONCE per script load, on the cold path, not per frame.

### 3. `MoonLiveEffect` answers from the loaded script

- `MoonLiveEffect::dimensions()` (the C++ override Layer calls) answers from the script's `dim()`,
  read once at load. This is a **behavior change**: Layer.h:228 extrudes on it, so a script
  declaring 1 now paints one column and gets duplicated rather than painting the grid itself.
- `tags()` returns the script's string, falling back to "📝" when absent, so the notepad still
  marks a script with nothing to say.
- Audit every shipped `.mle` for its true dimensionality and declare it. Most are 2D (unchanged);
  the audit is what stops a wrong declaration reaching a wall.

### 4. The catalog carries dim and tags

`catalog_scripts.py` emits names only, deliberately (~12 bytes vs ~800). Two extra fields per
entry is a byte or two each, which keeps that property.

The generator must learn dim and tags **without running the script**, so it parses the two
functions out of the source. That is a second reader of the language, which is the duplication the
architecture rule targets, so it is bounded deliberately:

- The parse is a regex for `int dimensions() { return <int>; }` / `string tags() { return "<str>"; }` in the
  class body, nothing more. A script whose declaration the generator cannot read is **a build
  failure**, not a silent default, so the two readers cannot disagree quietly.
- The catalog value is a **hint for the picker only**. Once a script is on the device, the compiled
  script is the truth: `MoonLiveEffect` never reads the catalog. So a stale catalog can mislabel a
  row in the picker and can never change what runs.

### 5. UI

- The script `<select>` (app.js:2355) renders each row with its emoji, the way the module picker's
  rows do, reading dim/tags from the catalog for undownloaded scripts and from the module for the
  loaded one.
- Dimension emoji reuse `DIM_EMOJI` (📏/🟦/🧊) so a script and a compiled effect read identically.
- A `<select>` cannot style its rows richly; if the emoji prefix in the option text is not enough,
  the picker becomes the same list-with-chips the type picker uses. Decide when it is visible,
  not now.

## Steps

1. `return` in the language: lexer, IR, four backends, `ValueFn`. Tests: a value returned from a
   helper, an early return from `tick()`, a bare return, per-backend codegen tests. **DONE**
1b. Declared return types (`void` / `int` / `string`), recorded per entry point and enforced at the
   call boundary; migrate the shipped scripts, tests and docs.
2. `dimensions()` / `tags()` read by `MoonLiveEffect` at load; fallbacks when absent.
3. Audit and annotate the shipped scripts.
4. Catalog generator parses both; a declaration it cannot read fails the build.
5. UI renders emoji per script row.
6. Docs: the MoonLive language page gains `return`, `dimensions()` and `tags()`; the effects doc
   notes that a script's dimension drives extrusion.

## Tests

- Language: return a value, early return, bare return, return inside a loop; one codegen test per
  backend (the return register differs per ISA and is the thing most likely to be wrong).
- Types: a `void` function's value is refused, a `string` that returns nothing is a compile error,
  a bare undeclared name is refused.
- Effect: a D1 script extrudes across a 2D layer; a script with no `dimensions()` still behaves as D2; a
  script's tags reach the module.
- String lifetime: tags read after re-render still point at valid text.
- Catalog: a script with declarations produces catalog entries carrying them; a malformed
  declaration fails the build.
- Every shipped script still compiles (the existing sweep already covers this).

## Verification

Desktop build + ctest. On hardware: an S3 running a D1 script on a 2D layout, extruded correctly,
and the picker showing per-script emoji. The extrusion change is visible on a wall, which is where
it must be judged.

## Risks

- **The extrusion change is the sharp edge.** A script that declares D1 but paints the whole grid
  renders differently than before. The audit in step 3 is what contains it; a wrong declaration is
  visible immediately on a wall.
- **Return-register codegen is per-ISA** and a mistake is silent (a plausible wrong number). The
  per-backend tests are not optional here.
- The catalog parser is a second reader of the language. Bounded to two fixed forms, failing the
  build rather than guessing.
