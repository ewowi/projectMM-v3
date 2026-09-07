# Coding standards

How code in this repo is written. Hard rules and process live in [CLAUDE.md](../CLAUDE.md); how to build and run lives in [building.md](building.md); what is tested lives in [testing.md](testing.md). Design rationale for the rules below lives in [architecture.md](architecture.md).

## Conventions

Decided once; not re-derived per file.

- **`#pragma once`** for header guards. No `#ifndef … #define … #endif`.
- **`constexpr`** over `#define` for compile-time constants. `#define` is reserved for build-system flags (e.g. `MM_NO_WIFI`) that need to be visible to the preprocessor.
- **`std::span`** over pointer + length pair in function signatures. The span carries the bound; the caller can't desync.
- **Namespace `mm`** for everything in the project. Platform code lives in `mm::platform`. Tests live in `mm` (no `mm::test` — keeps the same names visible to test code as to library code).
- **No `using namespace` in headers.** In a `.cpp`, `using namespace mm;` is allowed at file scope. In a header it pollutes every translation unit that includes it.
- **Semantic variable names.** Name variables for what they represent, not just their type. `availableHeap` not `available`, `internalHeap` not `internal`, `lutBytes` not `bytes`. A reader should understand the variable without looking at its assignment.
- **No hard line wraps in markdown.** Let the editor soft-wrap. Hard wraps make diffs noisier than they need to be.
- **No em-dashes (` — `) in prose.** Use a comma, semicolon, colon, parentheses, or a full stop instead, whichever the clause actually calls for. Applies to docs, comments, and commit messages. (Code is exempt: a literal `—` in a UI string or test fixture stays.) Existing em-dashes get replaced as files are touched, not in a single sweep.
- **A conditional control is registered BELOW the control it depends on.** When a control's visibility (or range, or presence) is gated on another control's value, `addX()` it *after* that control in `defineDriverControls()`/`defineControls()`, so registration order matches the dependency: the driving control first, the dependent one under it. This makes the UI read top-down as cause then effect (toggle `pinExpander` on, the `latchPin` that depends on it appears just below), and it keeps the source order self-documenting: a reader sees the gate, then the thing it gates. Controls that are mutually exclusive (never visible together) still follow the rule by their shared gate, grouped after it.
- **American English spelling, everywhere.** Code identifiers, JSON/wire keys, comments, docs, and UI strings all use US spelling: `color` (not the British form), `serialize`, `optimize`, `initialize`, `normalize`, `behavior`, `center`, `gray`, `canceled`, `analyze`. Two reasons: (1) it's the dominant technical-project convention (LLVM, Chromium, the Linux kernel are American throughout), and (2) the graphics/LED ecosystem we interop with is uniformly American (`CRGB`, `color`, `colorFromPalette`, CSS `color`), so a British identifier fights the whole toolchain. The real hazard mixing creates: a grep for one dialect silently misses the other, and a wire key that drifts between dialects breaks a cross-device contract without a compile error. So one dialect, chosen to match the ecosystem. Watch-outs: `analysis` is already US (keep it; only `analyse`→`analyze`); a proper noun keeps its own spelling (`Travelrouter`, a product name). Existing British spellings in older prose get converted **opportunistically when a file is touched**, not in one big sweep (same as the em-dash rule above); a code identifier or wire key in the wrong dialect is the higher-priority fix, since it's a correctness hazard, not just style.
- **All Python through `uv run`.** Never bare `python`/`python3`: not in shell commands, not in CMake, not in docs. uv manages the project venv and is the project standard ([moondeck/MoonDeck.md](../moondeck/MoonDeck.md)); bare `python3` isn't on PATH on Windows, and the macOS Python launcher pops a Store prompt. In CMake, resolve `find_program(UV_EXECUTABLE NAMES uv REQUIRED HINTS "$ENV{USERPROFILE}/.local/bin" "$ENV{HOME}/.local/bin")` once and use `${UV_EXECUTABLE} run python …` thereafter; the shared `src/ui/embed_ui.cmake` takes a `PYTHON_CMD` parameter (desktop passes uv; ESP32 passes IDF's Python). The one exception is `esp32/main/CMakeLists.txt`: ESP-IDF builds use IDF's own bundled Python venv via `find_package(Python3)`, since IDF manages that environment itself.
- **Consider extending before creating.** When adding a feature, check whether an existing module extends cleanly; a new file is fine if genuinely cleaner, but justify it.
- **Do not remove comments** unless they are outdated or factually wrong. Comments document intent and context; removing them silently loses knowledge.
- **Reference, don't copy.** Prior art (friend repos, datasheets, our own prototype branches) holds proven approaches: study it, take the ideas, write our own code, never copy or trace the structure. Credits live in the [friend-repo digests](friend-repos/README.md) and per-module prior-art sections.
- **Minimal comments in MoonLive scripts, in exactly three places.** A `.mle`/`.mll`/`.mlm`/`.mls`/`.mlp` is a user-facing artifact shown in an editor on the device's own card, not a C++ source file: the reader is looking at the effect, and a comment block longer than the code buries it. So the budget is fixed, and it is one line each:

    | where | what it says |
    |---|---|
    | **one line at the top** | what the script IS, in a phrase a user would recognize |
    | **one line after each `addControl`** | what that knob does, from the user's side |
    | **one line before each function the script defines itself** | what that helper does, so a reader need not decode it |

    The third is the one to get right as scripts grow helpers: a named function is a promise about what it does, and the line is where that promise is written. Lifecycle functions (`tick`, `defineControls`, `placeLights`, `modifyLogical`, …) need no line: their names are the contract and the reader already knows them.

    Everything else, the reasoning behind a formulation, the measured numbers, the language limits it works around, belongs in the commit message or the roadmap. This is the one place the "do not remove comments" rule above yields: on these files, trim.
- **Present-tense litmus.** "There is no MCLK pin" states a property (keep); "no X anymore" narrates a removal (cut it; describe the path that exists).

## Prefer integers, store values in their native shape

**Default to integers.** Pick the smallest unsigned type that fits the natural range — `uint8_t` for percentages and small counts, `uint16_t` for pixels and ports, `uint32_t` for timestamps and byte counts, `int8_t` for signed RSSI-style values. Integers are faster, predictable, branch-free in the hot path, and one byte where they need to be.

**Use `float` only when the value is intrinsically fractional** — geometry positions on a normalised grid, audio amplitudes, ratios that would lose meaning if rounded. Even then, ask whether scaled integers (e.g. fixed-point `uint16_t` for 0..65535 mapping to 0.0..1.0) work. The render loop hits every light every frame; an integer multiply-and-shift dominates a float multiply on ESP32.

**Never use `double`** in firmware code. Xtensa (ESP32 classic) has no hardware FPU for `double` — every `double` operation runs in software emulation, ~30× slower than the same op on `float`. `1.0` is a `double` literal; write `1.0f` if you really meant float. A `double` slipping into the render path silently tanks FPS.

**Store values in their native shape.** When the value is intrinsically numeric, store it as a number. When it is four octets, store it as four octets. **Don't keep a long-lived string buffer just because the UI eventually shows the value as text** — format to string at the output boundary, on the stack, then throw the buffer away. Every "I'll just `char foo[12]` it now and snprintf into it" decision freezes a few bytes into the module's permanent footprint, where the cheaper alternative is one int and a `snprintf` on a local stack buffer at serialization time. On ESP32 with ~180 KB free heap, dozens of those add up.

Guidelines:

- **RSSI, TX power, frame counts, percentages, temperatures, voltages** — store as `int8_t` / `uint8_t` / `uint16_t`. If the UI needs a unit suffix, carry the suffix in the control descriptor (`ControlType::ReadOnlyInt` does this — see [Control.h](../src/core/Control.h)), not in a per-instance string.
- **IPv4 addresses** — store *and* pass as `uint8_t[4]`, not `char[16]` dotted-quad. The convention runs through the whole seam, not just storage: a getter that returns a string (e.g. a platform `getIP(char*)`) forces every caller into a `char[16]` and the string form leaks upward — one caller even round-trips it back to octets (string → `parseDottedQuad` → bytes) to do byte-level work. So the platform IP getters return octets (`ethGetIPv4(uint8_t[4])` / `wifiStaGetIPv4`); each consumer formats with `formatDottedQuad` at its own output boundary (text), or uses the bytes directly (ArtNet, comparisons). The wire/JSON format stays a string at that boundary only; storage and transfer stay 4 bytes. See `ControlType::IPv4` and `formatDottedQuad` for the pattern.
- **Mode / status labels from a small fixed set** — a `char[20]` buffer is acceptable when the label is short and `snprintf`'d at a transition; for purely constant labels (`"Idle"`, `"Connected"`) a `const char*` pointed at a static literal is even cheaper. Don't combine the two: don't `snprintf` a literal into a buffer.
- **Dynamic display strings (uptime, FPS, heap KB)** — `char[N]` buffer is the established pattern (see [SystemModule.h](../src/core/SystemModule.h)) because the value changes every second and the UI reads it by stable pointer. Size the buffer to the longest possible value; oversized buffers are waste.

Counter-example to avoid: storing `char rssiStr_[12]` and re-`snprintf`'ing `"-58 dBm"` into it every tick. The right shape is `int8_t rssi_` (1 byte) plus a control type that knows the unit. Saves 11 bytes per metric, scales linearly across the codebase.

**Width the intermediate, not just the result.** Any `a * b` where both operands are `nrOfLightsType` (or a count times a multiplier) can overflow `uint16_t` even when each operand is individually small (`256 * 256 = 65536` wraps to 0 on a no-PSRAM device); do the arithmetic in a wider type (`uint64_t`), clamp to the ceiling, then narrow. Likewise a counter derived from a cell count (a delta, a stagnation check) must be the domain typedef (`nrOfLightsType`), not a fixed `uint16_t`. A width-sensitive path is invisible on the uint32 desktop build, so pin it with a `uint16`-typed unit test or hardware confirmation.

**When a validation field's storage is narrower than what it claims to validate, the validation is wrong, not the field.** A `uint8_t` min/max slot can't bound an `Int16` control (it clamps to `[0..0]`); the fix is a wider bound (or per-type bound slots), and until then the constraint is documented at the field's declaration.

**When one control type does two jobs with different UX, that's the smell for a new type, not a range hack.** An `int16` control the UI renders as a slider can't also mean "GPIO pin number"; a dedicated `Pin` type (smallest storage that fits the domain, `int8_t` for a GPIO) is the fix, not overloading the range.

## Animate on elapsed time, never on the frame count

**A faster device renders the same motion more smoothly, not more motion.** Anything on a tick path
whose output changes between two calls with identical inputs is animating, and it takes its step
from wallclock, not from having been called. This holds for effects, for modifiers that scroll or
rotate, and for anything else the render loop reaches. A pure fold of coordinates from controls is
not animating and owes nothing. Rationale and the two-rate check:
[architecture.md](architecture.md#live-reconfiguration-every-change-applies-without-a-reboot).

The shape, whichever quantity it is:

```cpp
carry_ += rate * time_.advance(elapsed());     // particles::FrameTime, 256 = one 1/60 s frame
uint32_t due = carry_ / particles::FrameTime::kOne;
carry_ -= due * particles::FrameTime::kOne;    // CARRY the remainder, never floor it to 1
```

**Carry the fraction.** At a high render rate the per-frame amount is legitimately below one unit,
and flooring it to 1 applies many times the intended amount: that is what made trails visibly
shorter on a fast device than on a slow one. Cap `due` so a long stall tops the effect up rather
than bursting a frame's worth of work at once.

**Ask whether something upstream already scales it.** Do not scale a quantity twice. A fade
requested through `Layer::fadeToBlackBy` is already scaled by the Layer, so an effect passes a rate
and does nothing else; a fade requested only on frames that pass a wallclock gate gets throttled by
the gate AND by the scale. When in doubt, write the two-rate test first.

**A compounding spatial operation is not a rate.** `draw::blur` applied twice at half strength is
not one blur at full strength, so the carry pattern above does not transfer to it. Gate it in time
and leave it at full strength.

## Per-type behaviour lives with the type

When a struct or enum is the semantic owner of some data — a control descriptor, a packet, a module role — the functions that interpret, serialise, validate, or otherwise operate on it should live next to the type, not at the call sites that use it. Free functions in the same `.cpp` count; member methods on the owning class are stronger; virtual methods on a base class are strongest. The wrong shape is the same `switch (type)` repeated in every consumer — adding a variant means hunting across N files for switches to extend, and the compiler can't tell you when one gets missed.

Three concrete patterns, all already common in this codebase:

- **Discriminator + free functions in the type's own file.** `ControlType` + `writeControlValue` / `applyControlValue` / `controlTypeName` in [Control.cpp](../src/core/Control.cpp); `parseDottedQuad` / `formatDottedQuad` next to `ControlType::IPv4` in [Control.h](../src/core/Control.h); `LightPreset` + `rebuild()` in [Correction.h](../src/light/drivers/Correction.h). Best when the discriminator is a plain enum and the operations are small.
- **Methods on the owning class.** [Buffer.h](../src/light/layers/Buffer.h)'s `allocate` / `free` / `clear`; [Scheduler.h](../src/core/Scheduler.h)'s `addModule` / `tick` / `prepareTree`; [ControlList](../src/core/Control.h)'s `addX` family. Best when the class has identity and the operations naturally form a small interface.
- **Virtual methods on a base class.** [MoonModule.h](../src/core/MoonModule.h)'s lifecycle (`setup`, `tick`, `tick1s`, `defineControls`, `prepare`, …). Best when polymorphism is already in play.

Counter-example to avoid: a `switch (c.type)` on `ControlType` duplicated in HttpServerModule, FilesystemModule, and scenario_runner. That shape forces a new ControlType to be added in four places, and the compiler can't catch a missed switch on a non-exhaustive enum. The per-type dispatch instead lives next to `ControlType` in [Control.cpp](../src/core/Control.cpp); consumers call `writeControlValue(sink, c)` and don't need to know the enum's shape.

When a `switch (type)` outside the type's home file is legitimate: the caller has a genuinely different concern (HttpServerModule mapping `ApplyResult` to HTTP status codes is a transport policy, not per-type behaviour; scenario_runner's `switch (JsonVal::type)` dispatches on *its own* discriminator, not `ControlType`). The rule is "per-type dispatch lives with the type", not "switches are banned".

**A flat parser that returns a zero sentinel for a missing key must be paired with a presence check before the value is applied as authoritative.** `json::parseInt(json, key)` returns 0 for an absent key, indistinguishable from a real 0; on a persistence-overlay load path that clobbers a non-zero default when an older/partial file omits the key. Guard with `json::hasKey()` first: an absent key leaves the control untouched (its default stands); a present key (even value 0) applies. Any "control resets to its default/0 after reboot" symptom is this overlay smell, not a control-init bug.

**A reader that decodes only a subset of the escapes its writer emits is a latent asymmetry bug.** If the writer emits `\n` / `\t` escapes, the reader must decode them (not only `\"` / `\\`); make the escape set symmetric so multi-line text round-trips.

## File shape: header-only vs `.h` + `.cpp`

- **Light-domain modules and the `MoonModule` base: header-only.** Every effect, modifier, driver, layout, the light-domain containers (`Layouts`, `Effects`, `Drivers`, `Layer`), and the `MoonModule` base class live in a single `.h` with implementation inline. The benefit is concrete: a contributor copies `RainbowEffect.h`, edits, saves as `MyEffect.h`, registers one line in `main.cpp` — no "where does the `.cpp` go, what does CMake need" friction. The chain `RainbowEffect.h → EffectBase.h → MoonModule.h` is uniform; readers don't pivot to a different file shape at the base. When a light-domain file outgrows one concern, extract a helper into its own header (`BlendMap`, `MappingLUT`) rather than splitting to `.h` + `.cpp`. Header-only is a feature of the light domain.
- **Core service modules: `.h` + `.cpp`.** Core modules that bridge to the platform layer or implement substantial infrastructure (`HttpServerModule`, `FilesystemModule`, `NetworkModule`, `Scheduler`, `SystemModule`, `Control`) ship as a `.h` (interface) plus a `.cpp` (implementation). Three reasons that compound: (a) implementation changes recompile only the `.cpp`, not every TU that includes the header — incremental builds are 2–5× faster on the kind of edits that happen in development; (b) readers want the interface separately from the body; (c) symbol bloat and link-time stay bounded. Small core utilities that are *almost entirely declarations or inline accessors* — `types.h`, `color.h`, `version.h`, `BinaryBroadcaster.h`, `JsonUtil.h`, `JsonSink.h`, `Sha1.h`, `Base64.h` — stay header-only. Templates (e.g. `ModuleFactory::registerType<T>`) also must stay in the header because of C++ instantiation rules; a module that's mostly template can therefore stay header-only.
- **A catalog module includes ONLY its base header.** Every effect, modifier, layout, and concrete driver leads with exactly one include — `light/effects/EffectBase.h`, `light/layouts/LayoutBase.h`, `light/modifiers/ModifierBase.h`, or `light/drivers/DriverBase.h` — the base class it subclasses, and nothing else at the top of the file. That base header is the module author's **standard library**: it declares the base class AND pulls in the render context, the common domain helpers (`draw` / `Palette` / `math8` / `noise` / `color` / `crc` for effects; `DriverBase`'s own `Layer`/`Buffer`/`Correction`/platform for drivers; the base + integer trig for modifiers/layouts), the lifecycle primitives (`ScratchBuffer`), the audio source, AND the standard-library headers the bodies use (`<cstring>`, `<cmath>`, `<cstdint>`, `<array>`; drivers add `<cstdio>`, `<algorithm>`). Bundling this whole surface is **byte-free** — unused declarations emit no code (measured: the ESP32 image did not grow when the set was maximised), so the reflex is *add the common header to the base, don't scatter it per file*. That keeps every module in a domain reading identically and the copy-edit-register workflow free of include guesswork; it's also the surface a scripted MoonLive module gets uniformly. This is a *maximal* (prelude-style) bundle on purpose — a recognisable pattern (Rust's `std::prelude`, a project-wide `framework.h`), justified at the introduction site in each base header's comment.

  **The only permitted second include** is a helper that is genuinely *outside* the domain's standard surface — a network packet format (`ArtNetPacket.h`), a font table (`fonts.h`), the module factory, a platform primitive (`platform.h`), a specialised core service (`JsonUtil.h`, `DevicesModule.h`) — and it carries a one-line justification comment so the exception is visibly deliberate. If the "extra" is a *common* header two-plus modules of the same kind reach for, it is not an exception: **move it into the base header instead** (that is how `<cstdint>`/`<array>`/`<algorithm>` got there). One structural subtlety, in `EffectBase.h` only: `Layer.h` (which defines EffectBase's out-of-line accessor bodies) includes `EffectBase.h` back, so it can't sit at the top — `EffectBase.h` forward-declares `Layer`, declares its accessors, and pulls `Layer.h` + the helper set at the **bottom of the file, after the class**, where `Layer.h` re-enters as a no-op (include guard) with EffectBase already complete. The standard forward-declare-then-include-the-definer pattern; the other three domains have no cycle and include their base + helpers top-down.
- **Exceptions need a one-line comment at the top of the file naming the reason.** Without a stated reason the file is expected to follow the default for its category. When in doubt: light → header-only, core → `.h` + `.cpp`.

## Override-and-chain convention

A MoonModule that owns children gets the standard lifecycle methods (`setup`, `tick`, `tick20ms`, `tick1s`, `release`, `defineControls`, `prepare`) propagated to children by the base class default — see [architecture.md § MoonModules](architecture.md#moonmodules). When a container overrides one of these to add its own work, the convention is **when in the override to call the base**:

- **`tick` / `tick20ms` / `tick1s`** — option A: parent work first, then chain. The parent prepares state that children consume.

  ```cpp
  void tick() override {
      if (layer_ && layer_->lut().hasLUT() && outputBuffer_.data()) {
          blendMap(...);                  // parent's own work
      }
      MoonModule::tick();                 // then tick children
  }
  ```

- **`setup`** — chain first, then parent work. Children must be initialised before the parent depends on them.
- **`defineControls`** — chain first, then parent work. Children register their controls before the parent appends or rewires its own. Lets a parent build a list whose order is "children's controls, then mine."
- **`prepare`** — chain first, then parent work. Children compute their dimensions and dynamic buffers before the parent reads or modifies the shared state (Layer reads child effect/modifier dimensions; Drivers reads Layer output sizing).
- **`release`** — parent work first, then chain. The parent shuts down its own state before the base reverse-iterates children.

Option B (children first on `tick`; parent first on `setup` / `defineControls` / `prepare`) or a sandwich pattern is allowed only when a specific reason justifies it; add a one-line comment at the override explaining why.

## Casts

Use project typedefs (`lengthType`, `nrOfLightsType`) consistently so types match and casts are unnecessary. When casts are needed:

- **`static_cast`** — converts a value between related types. Checked at compile time. Use only at system boundaries: byte protocol packing, OS API return values, overflow-prevention with wider intermediates. If you need `static_cast` between project types, make the types match instead.
- **`reinterpret_cast`** — reinterprets raw memory as a different type. No conversion, no safety. Avoid. The only legitimate use is raw byte / memory access (e.g. `reinterpret_cast<const sockaddr*>` for socket APIs).
- **`dynamic_cast`** — runtime-checked cast from base to derived. Requires RTTI, disabled on ESP32 (`-fno-rtti`). Not used.

## Compiler warnings

All targets build warnings-as-errors: `-Wall -Wextra -Werror` on Clang/GCC (macOS, Linux, ESP32), `/W4 /WX` on MSVC (Windows) — gated by compiler in `CMakeLists.txt`. No warning is "harmless" — fix it or silence it explicitly with a `-Wno-…` (Clang/GCC) or `#pragma warning` justified in code.

**A clean local build is not proof the Windows build passes.** MSVC's `/W4` flags things Clang/GCC's `-Wall -Wextra` don't — most commonly **signed/unsigned mismatch in comparisons** (C4389), e.g. `(x & 1) == 1u` where the left side is signed: harmless logically, fatal under `/WX`. This has slipped past the macOS gate and broken Windows CI more than once. So:

- In a comparison, keep both sides the same signedness — don't mix a signed expression with an unsigned literal (`== 1`, not `== 1u`, when the other side is signed). Watch `& `, `%`, and subtraction results, which carry the signedness of their operands.
- A change that only built+passed on macOS/Linux is **not** verified for Windows. The Windows CI job (`release.yml`) is the real gate for MSVC-only warnings; let it run before considering a `src/`-touching change done, or build with MSVC locally if you have it.

## Editor setup (clangd)

Diagnostics appear **as you type**, from the same [`.clang-tidy`](../.clang-tidy) config CI
uses — so a finding shows up while the code is still in your head, not ten minutes later in a
pipeline.

Once per machine: install the **clangd** extension (`llvm-vs-code-extensions.vscode-clangd`)
and **disable Microsoft's C/C++ IntelliSense** — running both produces duplicated and
contradictory diagnostics. Nothing else to configure: [`.clangd`](../.clangd) at the repo root
points at the compilation database, and `CMAKE_EXPORT_COMPILE_COMMANDS` (set in
`CMakeLists.txt`) means any normal build refreshes it.

Two things worth knowing:

- **If every file reports `'cstdint' file not found`**, the build directory was configured
  with a different compiler than clangd is. `.clangd`'s `--query-driver` handles the usual
  cases; if a new toolchain appears, add it there. This failure is loud and total — real
  diagnostics disappear behind it — so it is worth recognising on sight.
- **clangd runs a subset of the CI check set**, skipping checks it considers slow (>10%
  AST-build cost). That is deliberate and means the same config file is safe to share: CI
  remains the authority.

## Static checks

- **Platform boundary** (`moondeck/check/check_platform_boundary.py`) — scans all files outside `src/platform/` for `#ifdef` / `#if defined` with platform macros and `#include` of platform-specific headers (`esp_*`, `freertos/*`, `driver/*`, `SDL.h`, `wiringPi.h`, …). Fails if any are found. The platform boundary rule itself: [architecture.md § Platform abstraction](architecture.md#platform-abstraction).
- **Hot path check** (`moondeck/check/check_nonblocking.py`) — `MoonModule::tick/tick20ms/tick1s` carry `MM_NONBLOCKING`, and Clang 20+ verifies under `-Wfunction-effects` that nothing they reach allocates or blocks — **transitively**, through the whole call graph. It reports; it does not fail a build: a new blocking call may be legitimate (a driver that must wait for hardware), so the finding is stated and the product owner judges it. `docs/metrics/hotpath-baseline.txt` freezes the known set so a new one stands out. The hot path rule itself: [architecture.md § Hot path discipline](architecture.md#hot-path-discipline). A "no blocking in the hot path" audit must sweep *every* syscall the path can reach (connect, DNS, read, *and* write), not just the loudest one: a single-threaded loop that services I/O must make no blocking call at all (a socket timeout is not a fix, it is the size of the freeze; non-blocking + poll is the only safe shape). Fixing one blocker while an equally-blocking sibling survives is a partial fix that reads as complete.
- **Code formatting** — `clang-format` with a project `.clang-format` file. Applied in CI; code that doesn't match fails the check. Run locally via editor integration or `clang-format -i`.

## When checks run

Which checks run at which lifecycle event is defined once, in the [Commit](../CLAUDE.md#commit), [Merge](../CLAUDE.md#merge) and [Release](../CLAUDE.md#release) tables: one command per check, each with an objective path trigger, so a change runs only the checks it makes applicable. CI runs the same checks on every PR ([.github/workflows/](../.github/workflows/)).

## Tests

- **Placement.** New core logic gets a module (unit) test; a full pipeline gets a scenario test. Inventory and strategy: [testing.md](testing.md).
- **Interim fixes.** When a per-module interim ships in place of the named core fix (see [CLAUDE.md § Principles](../CLAUDE.md#principles), Architecture first), its tests assert *behavior*, not the per-module mechanism, so they survive the later move into core unchanged.

## Documentation model

Where each kind of fact lives. The guiding rule: **document a thing once, in the place closest to the thing, and generate or link the rest** — a fact the source states is never re-typed in prose. Every module has exactly **two reader surfaces**: a hand-written **summary page** (end-user, a table) and a **generated technical page** (from the `.h`). This is the docs.rs / Sphinx-autodoc / Doxygen split — a hand-written guide over a generated API reference — a pattern a web/systems contributor recognises on sight.

Two corollaries. **Reference a module generically** in prose outside its own home: say "a modifier" or "a driver", never `FooModifier`; a module's one home is its `.h`, its catalog card, its registration, and its tests, and naming it anywhere else multiplies rename cost while teaching nothing a link wouldn't. And the **spec lifecycle**: when a module ships, its final spec lands in `docs/moonmodules/<Name>.md` and any backlog draft is deleted; most small modules skip the draft and go straight there.

### The tree mirrors `src/`

[Everything is a MoonModule](architecture.md#moonmodules); `docs/moonmodules/` mirrors `src/`, a **`core/`** subtree and a **`light/`** subtree. Within each, a module is either **UI** (user-facing and configurable — the System modules like Network/System/FileSystem, the user-added [Services](architecture.md#services), and the light **catalog modules**: layouts, effects, modifiers, drivers) or **supporting** (the machinery UI modules lean on — `Control`, `Scheduler`, `Layer`, `Buffer`, the `*Base` classes). [Services](architecture.md#services) are UI modules that bridge to hardware (a sensor, a network integration).

### The two surfaces

1. **Summary page (hand-written, end-user).** One `.md` per module *group*, a 4-column table — **name + description · gif/image · controls · links**. One row per module, authored as `###` prose blocks that a build-time hook ([`moondeck/docs/mkdocs_hooks.py`](../moondeck/docs/mkdocs_hooks.py)) renders as the table. Catalog controls live here because a catalog module's user surface is its runtime `controls_.add(...)` calls, which no static tool sees. Each group is one flat page under its domain (a group's `type` rides in the *page name*, not a subfolder — the [folder-structure decision](adr/0015-library-is-a-tag-not-a-folder.md)):
   - `light/{effects,modifiers,layouts,drivers,supporting}.md` — the light-catalog + light-supporting pages (a type may later split by library into `effects_wled.md` / `effects_moonmodules.md`, still flat).
   - `core/{services,supporting,ui}.md` — the core-services (user-facing modules), core-supporting, and web-UI summary pages.

   **The Links column** is assembled by the hook in a fixed order (**🧪 Tests · 📄 Technical · attribution · ⌄ details**), each with a Material icon (`:material-…:`, rendered as inline SVG by `pymdownx.emoji`, the same mechanism as the tag emoji in the Name column) so a link's *type* is scannable. A card carries these lines: a `[Tests](../../tests/unit-tests.md#<anchor>)` line (omitted when the module has no unit test, so a missing Tests link truthfully means "untested"), a **`Detail: [technical](../moxygen/<Stem>.md)`** line pointing at the generated technical page, and an `Origin:` attribution line. `check_specs` matches each block to its `.h` via that `moxygen/<Stem>.md` link, so keep the link's target on it.

   Cross-file design rationale that no single `.h` owns (module interactions, buffer-lifecycle coupling) is a prose section beneath a summary page's table — a `## <Name> — details` section the hook links from the row as `⌄ details`. That's the only home for it, so a module needs no page of its own.

2. **Technical page (generated).** `docs/moonmodules/{core,light}/moxygen/<Module>.md`, produced from the `.h` by [`moondeck/docs/gen_api.py`](../moondeck/docs/gen_api.py): **Doxygen** (the de-facto-standard C++ parser) emits XML, **moxygen** renders Markdown through a custom Handlebars template ([`moondeck/docs/moxygen-templates/`](../moondeck/docs/moxygen-templates/)). Each page carries the module's **description, variables, and members** from their `///` comments and links to its `.h`; the summary page's per-module `Detail: [technical]` link points here. Two shaping levers beyond raw moxygen, both to keep the page lean:
   - **Template** (`class.md`): the base class is a one-line `> **Inherits:** [Base]` link — the full inherited-member list is *not* re-dumped on every subclass (it lives once on the base's own page; re-listing `MoonModule`/`EffectBase`'s large surface everywhere is bloat — *No duplication*).
   - **Post-process** in `gen_api.py`: three plain-text directives survive Doxygen's `GENERATE_HTML=NO` XML (which drops `\image`/`@htmlonly`/raw `<img>` and relative links but keeps plain text) and are rendered here — **`@card <file.png>`** → an `<img>` of the module's UI-card screenshot (a missing asset drops the directive, no broken link); **`@moreinfo`** → splits the class description, relocating everything after it to a `## More info` section below the member lists (the deep-dive-at-the-bottom shape); **`@xref{anchor|label}`** → a page-local `[label](#anchor)` link (deliberately not `@ref`, which is a real Doxygen command). A further post-process wraps each member signature's declared **name** in `<code class="mm-sig">…<span class="mm-sig-name">name</span>…</code>` so the theme ([`extra.css`](assets/extra.css)) can highlight the name (accent, bold) while the type and arguments stay muted — moxygen emits a flat `<code>` string with no internal markup, so 'color only the name' can't be done in CSS alone. Site-wide, `extra.css` also colors `h1`/`h2`/`h3` with the theme's primary/accent (the slate theme otherwise renders every heading the same near-white, flattening the hierarchy).

   The pages are **gitignored, regenerated on build** (flipping to committed-and-drift-gated is a one-line `.gitignore` change plus a gate like `check_firmwares.py`, if PR-review of the generated output ever earns it). Doxygen (a brew/apt binary) and moxygen (via npx) are the one justified non-uv dependency, like ESP-IDF's Python (see CLAUDE.md); absent locally the pages skip and the rest of the site builds, present in CI they render.

### `///` comments are the single home for technical detail

A module's per-entity detail — the module description, each variable, each member — is a `///` / `///<` comment in the `.h`. It generates into the technical page and shows on IDE hover. Rules that decide what actually reaches the generated page:

- **`///`, not `//`.** Only `///` (Doxygen) comments generate; a plain `//` comment is invisible on the technical page (it's an implementation note for the source reader only). Class-level rationale you want documented goes in `///`.
- **Distribute detail to the member it describes; keep the class comment compact.** The class `///` states what the module *is* and its one defining contract — not a wall of per-control / per-method prose. Every public attribute (the config controls) and every public method gets its OWN `///`, leading with a tight one-sentence brief (the generated page shows that first sentence as the member's summary, so the reader scans a list of named declarations each with its purpose). Detail that belongs to one control (its range, what changing it does) lives on that control; detail that belongs to one method (its lifecycle role, its guarantees) lives on that method. This spreads the same information across the entities it documents instead of piling it into the class blurb, so nothing is repeated and each member is self-describing on hover and on the page. Don't restate what a superclass already documents (a `DriverBase` hook's contract lives on `DriverBase`, not re-explained on every driver). Compact, dense prose — relocate detail, don't delete it.
- **Deep dives go under `@moreinfo`, referenced from the brief — never bloat the lead.** When a class or a member genuinely needs *more* than its compact description (a mechanism explainer, a wiring diagram, a legend table for a diagnostic control), it does NOT swell the lead comment. Put an `@moreinfo` line in the **class** `///`; everything after it is deep-dive reference. Doxygen renders the class description *before* the attribute/method lists (a fixed order with no trailing slot), so a post-process (`gen_api.py`) relocates the `@moreinfo` tail to a **`## More info`** section at the bottom of the generated page, below the members — the reader gets a lean lead, the depth is one scroll away. The lead (and any member brief) then **refers** to it with an in-page link: `@xref{<heading-slug>|<label>}` (see the links bullet below). This keeps briefs short and each declaration scannable while nothing is lost — the same *relocate detail, don't delete it* rule, applied to the whole-class scale. (Physically the `@moreinfo` block sits at the END of the class `///`, before `class X` — an end-of-*file* block can't work: Doxygen drops a `///` detached from what it documents, and `\class`-attaching one corrupts the brief/detail split and loses IDE hover.)
- **The class comment must sit directly above the class**, inside `namespace mm {` — a `///` block separated from `class X` by the `#include`s or the namespace open is detached and Doxygen drops it (its whole description then goes missing from the page). Put the includes first, then `namespace mm {`, then the `///` block, then the class.
- **Relative `.md`/doc links are stripped** by Doxygen's XML-only mode (only `http(s)://` links survive). Don't put `[text](../some.md)` in a `///` comment — link with a full URL, or name the target in prose (e.g. "see the LED signal-integrity use-case guide"). For an **in-page** link (to a `@moreinfo` subsection on the same generated page) use **`@xref{<anchor>|<label>}`** — `<anchor>` is the target heading's MkDocs slug (lowercase, spaces→`-`, punctuation dropped), `<label>` the link text (omit `|<label>` to use the anchor as the text). It survives Doxygen as plain text (it is NOT `@ref`, a real Doxygen command that mangles it) and a post-process turns it into `[label](#anchor)`. Put it in the member's **first sentence** if the link must show in the attribute/method table (that table renders only the brief).
- A bare `<tag>` renders as a live HTML element and swallows the rest of the page (wrap any `<…>` in backticks), and `JAVADOC_AUTOBRIEF` ends the brief at the first `.` (write "such as", not "e.g.").
- Where a comment would hand-copy a wire format / enum / constant, embed the real source with a `--8<--` snippet (`// --8<-- [start:name]`). Add `@card <file.png>` to show the module's UI card.

A module's story therefore lives in exactly two places: its `.h` (technical, generated) and its group summary row (end-user), which links to the technical page. Prior-art credit points at *other* projects learned from (FastLED, WLED, MoonLight, datasheets); superseded internal prototypes are not linked.

**No per-module detail `.md`.** A module's technical detail lives in its `.h` `///` (→ the generated page), never a separate hand-written detail page. When a common rationale repeats across sibling modules (e.g. the shared parallel-LED-driver body), it lives once on the shared base class's `///` and the siblings reference it — the same *No duplication* rule, applied to the generated pages.

### Test inventories: their own generator, not moxygen

`docs/tests/*.md` is generated by [`moondeck/docs/generate_test_docs.py`](../moondeck/docs/generate_test_docs.py), not moxygen. Unit tests are `TEST_CASE("…")` macros tagged with `// @module` and per-case `//` descriptions — a convention Doxygen documents nothing of (it parses C++ *entities*, not macro string literals) — and scenario tests are JSON, which moxygen cannot read. moxygen is for the `.h` module pages; the test generator owns the test pages.

## Defaults

**Assign a default only where the hardware, not the user's soldering iron, fixes the value.** The test is *who fixes the pin/setting*:

- **Chip-/board-fixed → default it, and you must.** The RMII Ethernet pin map, the on-board status LED, a country code per region: silicon-/PCB-wired, so a default cannot do harm, and *omitting* it does (a no-WiFi board with un-defaulted Ethernet pins can never connect to be configured, a chicken-and-egg lockout).
- **User-soldered → leave it unset.** A MEMS mic, an LED strand, an LED-driver pin goes wherever the user ran the wire, so any default is a guess that can drive a pin the user committed elsewhere. Empty until set; idle with a "set pins" status meanwhile (degraded is fine, crashed is not).

A "default" that is really one specific board's values is bespoke masquerading as standard (a § Principles violation): make the capability opt-in and require each consumer to state its own values, so a missing declaration fails loudly instead of inheriting a stranger's wiring. Never auto-run a peripheral whose init can block on absent hardware. The design rationale (the MCU → deviceModel provenance model) lives in [architecture.md § Config provenance](architecture.md#config-provenance-mcu-devicemodel).

## Debugging and verification

Hard-won discipline for diagnosing hardware and infrastructure failures, distilled from the war stories in [lessons.md](history/lessons.md).

- **Prove the failure is *about* the change before editing code.** When something fails right after a change, re-run it isolated, probe the actual end state (ping/curl the device, read the real CI error line, check machine load), and confirm the artifact under test is the one you built (process uptime, `build` timestamp, what is bound to the port). A stale process, a loaded machine, or an async-confirmation timeout reads as a regression it isn't.
- **A status/dimension assertion does not prove the pipeline renders.** A correctness test for a mapping or effect asserts the buffer or LUT is non-empty with the expected coverage (e.g. LUT destinations == physical light count), not just that the declared dimensions look right.
- **For a hardware bring-up, "it compiles" tells you almost nothing.** The truth is in the boot log on the actual board; a min-revision trap, a wrong PHY pin, a filename-keyed capability gate, or a Kconfig *choice* that an incremental build silently keeps all build clean and fail only on hardware (`rm -rf` the build dir when a sdkconfig choice changes).
- **When test and reality disagree, enumerate what the test abstracts away and make the test transmit the genuine article.** Each closed gap either finds the bug or eliminates a theory with proof (a whole-frame loopback that sends the driver's real frame, not a synthetic pattern). Prove the firmware is *not* the cause with a measurement at each layer before editing code or buying parts.
- **A measurement tool must be faithful to the real client, or it invents and hides bugs.** A one-shot WebSocket probe that gives up on close reports stalls a reconnecting browser never sees and misses blips it does; match the client's real behaviour (keepalive ping, auto-reconnect).
- **Stop at the first failed fix on a working path.** Revert to the known-good state at attempt two rather than re-engineering a seam that already worked (§ *Anti-stalling* in CLAUDE.md).
- **A generated artifact's ground truth is the rendered output, not a re-derivation.** Verify a doc anchor against the built HTML (`grep id=` the `.html`), not a reimplementation of the slug algorithm; verify an emitted machine instruction against the real toolchain's disassembler before flashing.
- **A cross-boundary fact duplicated in code drifts silently; gate it.** A `docPath` in `main.cpp` that points at a docs page, a firmware projection that mirrors a build dict: the moment a check can resolve it against ground truth, that check is cheaper than the drift.
