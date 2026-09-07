# Plan — Rename the module hooks to the industry `prepare / tick / release` spine

> **DECIDED (product owner): Option E.** After the industry-standard survey below, the module lifecycle hooks are renamed to the recognizable prepare/tick/release vocabulary that JUCE, Unity, Unreal, and the ESP-IDF/FreeRTOS runtime already use. This is a from-scratch project choosing best-practice names now, so there is no legacy to preserve — "no tech debt already." The rename is a mechanical, behaviour-neutral sweep (execution notes at the end); it applies to **every** module, core and catalog alike.

## Decision summary

| Current | New | One-line reason |
|---|---|---|
| `onBuildControls()` | `defineControls()` | "Declare my parameters" — a bare declaration verb, visibly distinct from state-building (the old `onBuild*` collision is gone). |
| `onBuildState()` | `prepare()` | JUCE's `prepareToPlay`, the exact model our doc already cited. "Prepare my derived state for the current config." |
| `loop()` | `tick()` | The per-frame word of the runtime (ESP-IDF/FreeRTOS ticks), our own scheduler (`Scheduler::tick()`, `tickTimeUs`), and the industry (Unreal `Tick`, Unity `Update`, JUCE `processBlock`). `loop` was the lone Arduino-sketch holdover. |
| `loop20ms()` / `loop1s()` | `tick20ms()` / `tick1s()` | Same family. |
| `teardown()` | `release()` | JUCE `releaseResources`. Reads as the visible opposite of `prepare` (every surveyed framework pairs acquire/release as opposites; ours didn't). |
| `onEnabled(bool)` | `onEnabled(bool)` — **keep** | A genuine *notification* ("you were just enabled"), so the `on` prefix is correct here. |
| `onUpdate(const char*)` | `onControlChanged(const char*)` | Also a notification, but names *what* changed instead of the vague `onUpdate`. |
| `setup()` | `setup()` — **keep** | Already the industry-standard term for one-time init (JUnit `setUp`, pytest `setup_method`, xUnit, Arduino). See § What stays and why. |
| `applyState()` | `applyState()` — **keep (internal)** | The core router, not author-facing; renaming it buys nothing. |

**Naming rules this set follows** (the two principles the survey established): **bare imperative verb = work the module does** (`prepare`, `tick`, `release`, `defineControls`); **`on…` prefix = a notification the module observes** (`onEnabled`, `onControlChanged`). Applying that split consistently is what removes the friction R4 named.

## Context

Writing the ["Build your own MoonModules" guide](../../../usecases/build-your-own-moonmodules.md) surfaced a naming concern (product-owner remark R4): the hook vocabulary a module author must learn isn't as friendly as it could be.

1. **`onBuildControls` vs `onBuildState` read almost identically** (both `onBuild…`) yet do very different things — declare UI controls vs build derived state/memory. The shared prefix *causes* the "wait, are these the same?" confusion the remark names.
2. **`onBuildState` (acquire) and `teardown` (release) are opposites but don't read as a pair** — nothing in the names signals "these two are the build/unbuild halves."
3. Some names are longer than they need to be for the friendliest possible author experience.

This is a **design study, not a commitment to rename.** The current names are load-bearing (every future module author reads them; every existing module uses them), so the bar for changing them is high and the decision is the product owner's. This plan lays out what exists, an **industry-standard survey** of how established frameworks name these hooks (§ Industry-standard survey), the research-grounded proposal it points to, and the trade-offs — so the choice is made with eyes open, not by a search-and-replace. The lineage is worth stating: we started with Arduino's `setup()`/`loop()`, evolved to `onBuildControls`/`onBuildState`/`teardown`, and this study checks that against best practice to land on the clearest names a newcomer understands.

## Verified current state

The full author-facing hook set on `MoonModule` ([MoonModule.h](../../../src/core/MoonModule.h)):

| Hook | Role | Runs when |
|---|---|---|
| `onBuildControls()` | Declare the UI controls (sliders/toggles) | Startup + when the control set changes |
| `setup()` | One-time, enabled-independent wiring | Once at boot (core/driver modules; **no effect uses it**) |
| `onBuildState()` | Build derived state / memory from control values | Boot + on grid/config change, via `applyState()` when effectively-enabled |
| `loop()` / `loop20ms()` / `loop1s()` | Per-tick work at three cadences | Every render tick / 20 ms / 1 s, while enabled |
| `teardown()` | Release everything the module holds | On disable/remove, via `applyState()` |
| `onEnabled(bool)` | Edge-triggered one-shot (rare) | Once per enable/disable transition |
| `onUpdate(const char*)` | Cheap per-control reaction | On any single control change |

**Blast radius of a rename:** ~85 `onBuildControls`, 27 `onBuildState`, 27 `teardown` overrides across `src/`, plus the base definitions, `Scheduler`/`applyState` call sites, the generated moxygen docs (from the `///` comments), `check_specs`, and the guide + architecture docs. It is a mechanical but *wide* change — and a **core-orchestrator contract change**, so it earns this plan per CLAUDE.md.

**Deliberate prior-art anchor (why the names are what they are).** `onBuildState`'s doc explicitly models it on **JUCE's `prepareToPlay`** and **UIKit's `layoutSubviews`** — framework-driven "set up your derived state for the current config" hooks. The verb "build" (not "rebuild") was chosen on purpose: the op is idempotent and history-agnostic. `onBuildControls` mirrors that ("build the surface vs build the state"). So the current names are a *considered* choice aligned with recognizable frameworks — *Common patterns first* — not an accident. Any rename must beat that bar, not just be shorter.

## Industry-standard survey (what real frameworks name these hooks)

The "a framework calls your object at defined lifecycle moments" pattern is old and widely solved. Surveying the frameworks whose model matches ours — an object is **declared**, **prepared once**, **ticked every frame**, and **released** — two naming traditions emerge, and our hooks straddle both (which is the inconsistency R4 feels).

**Our closest analog is a render/processing loop (prepare → process-per-block → release), not a DOM mount/unmount.** So JUCE and the game engines are the most load-bearing precedents; the web frameworks inform the *prefix* question.

| Our hook (semantics) | JUCE (audio) | Unity | Unreal | Vue 3 (Composition) | Angular | React (class) |
|---|---|---|---|---|---|---|
| **build derived state / memory for the current config** (acquire) | `prepareToPlay()` | `Awake()` / `Start()` | `BeginPlay()` | `onMounted()` | `ngOnInit()` | `componentDidMount()` |
| **per-frame work** | `processBlock()` | `Update()` | `Tick()` | *(n/a — render is declarative)* | *(n/a)* | `render()` |
| **release everything** (unacquire) | `releaseResources()` | `OnDestroy()` | `EndPlay()` | `onUnmounted()` | `ngOnDestroy()` | `componentWillUnmount()` |
| **declare controls/parameters** | *(AudioProcessorValueTreeState — a separate params object)* | *(serialized fields / `[SerializeField]`)* | *(`UPROPERTY` macros)* | *(declared in `setup()`/`props`)* | *(`@Input()` decorators)* | *(props)* |
| **react to one input change** | `parameterChanged()` | `OnValidate()` | `OnConstruction()` | `watch()` | `ngOnChanges()` | `componentDidUpdate()` |

**Findings that bear directly on our names:**

1. **The prepare/process/release trio is the strongest, most recognizable pattern** — JUCE (`prepareToPlay`/`processBlock`/`releaseResources`) is the textbook match for our `onBuildState`/`loop`/`teardown`, and our `onBuildState` doc *already* cites it. Every framework pairs acquire and release with names that *read as opposites* (`prepareToPlay`↔`releaseResources`, `Awake`↔`OnDestroy`, `onMounted`↔`onUnmounted`, `BeginPlay`↔`EndPlay`). **Ours don't:** `onBuildState` ↔ `teardown` share no root. That's the concrete R4 gap the survey confirms.
2. **The `on` prefix means "you're being notified of an event," not "do this work."** Vue's `onMounted`, Angular's `ngOn*`, and React's event props all use `on…` for *notification* callbacks. Our `onBuildState`/`onBuildControls` use `on…` but describe *work the module performs*, not an event it observes — a subtle semantic mismatch. JUCE/Unity/Unreal use **bare imperative verbs** (`prepareToPlay`, `Update`, `Tick`, `BeginPlay`) for work-you-implement, reserving nothing for a prefix. By that convention, work-hooks should be bare verbs; only genuine *notifications* (`onEnabled` — "you were just enabled") keep the `on`.
3. **Declaring controls is universally a *separate* concern from preparing state** — every framework keeps "what are my parameters" apart from "prepare my DSP/render state" (JUCE's ValueTreeState vs `prepareToPlay`; Unreal's `UPROPERTY` vs `BeginPlay`). So splitting `onBuildControls` from `onBuildState` is *correct* — the problem is only that the shared `onBuild*` prefix hides that they're different concerns. The fix is to make the names *look* as different as they *are*.
4. **`loop()` is the Arduino inheritance and the odd one out.** JUCE `processBlock`, Unity `Update`, Unreal `Tick` — the industry word for "the per-frame callback" is **`tick`** or **`update`**, not `loop` (a `loop` is what *contains* ticks). Our three cadences (`loop`/`loop20ms`/`loop1s`) would read more naturally as `tick`/`tick20ms`/`tick1s`.

### Why `tick` (the name we scrutinized most)

`tick` was the one rename to interrogate hardest, because ESP32 developers are used to Arduino's `loop()`. The case for it is unusually strong *for this project specifically*, on four independent axes:

- **`tick` is native to the runtime.** projectMM's firmware runs on **ESP-IDF, whose kernel *is* FreeRTOS** (a bundled component; `app_main` is a FreeRTOS task, the scheduler is running before your code starts). In that world **`tick` is the fundamental unit of time**: `CONFIG_FREERTOS_HZ` (the tick rate), `xTaskGetTickCount`, `vTaskDelay(ticks)`, `pdMS_TO_TICKS`, `TickType_t`. Arduino's `loop()` is a *sketch* abstraction layered on top of that — the layer projectMM is above.
- **The project's own ESP32 code already speaks it.** 11+ `pdMS_TO_TICKS(...)` calls across `src/platform/esp32/` — every delay/timeout is already expressed in ESP-IDF ticks. `tick` isn't imported; it's the word the platform layer already uses.
- **The codebase already renamed the concept everywhere but the hook.** `Scheduler::tick()` is the method that *calls* the per-frame hook; the metric is `tickTimeUs_`; the KPI line is `tick:132us(FPS:…)`; the docs say "render tick" ~28×. `loop()` was the lone survivor, creating the jarring seam `Scheduler::tick()` → `module->loop()`. After the rename it's `scheduler.tick()` → `module->tick()`, one word top to bottom.
- **`loop` is semantically wrong.** A *loop* is the `while(1)` that *contains* frames; a *tick* is one iteration of it. The hook is one frame, called by the loop — so `tick()` ("do one frame") is precise where `loop()` ("...the whole loop?") misleads.

**Will a beginner / effect writer understand `tick`?** Yes — and it teaches the *correct* model faster than `loop` does. The #1 beginner mistake with a render callback is writing their own `while` loop inside it (which hangs the device); the name `loop()` actively invites that. `tick()` blocks it at the name — a clock/metronome/heartbeat *ticks once, then again*, so no one writes a loop inside a `tick()`. The everyday clock metaphor lands with zero RTOS knowledge, and the guide introduces it in one line ("`tick()` — runs once per frame, like a tick of a clock; do a little each time, don't write your own loop"). The familiar-but-misleading name loses to the clear-but-slightly-new one — the same reasoning that retired Arduino's catch-all `setup()`.

## Research-grounded proposal (candidate vocabulary)

Mapping the survey onto our semantics, the most industry-aligned, self-consistent set — bare verbs for work, `on` only for notifications, acquire/release as visible opposites — is:

| Current | Proposed | Rationale (from the survey) |
|---|---|---|
| `onBuildControls()` | `defineControls()` | "Declare my parameters." Bare verb; `define` reads as declaration, clearly distinct from state-building. (cf. UPROPERTY/ValueTreeState as a *separate* concern.) |
| `onBuildState()` | `prepare()` | The JUCE `prepareToPlay` verb, shortened. "Prepare my derived state for the current config." Bare imperative = work-you-do. |
| `teardown()` | `release()` | JUCE `releaseResources`. Reads as the opposite of `prepare` (prepare↔release is a recognized pair). **Note:** collides with the `ScratchBuffer` plan's private `release()` helper — but that helper *disappears* under that plan, so the collision resolves itself. |
| `loop()` | `tick()` | Unity/Unreal/JUCE per-frame word. `tick`/`tick20ms`/`tick1s` reads as "the periodic callback." |
| `loop20ms()` / `loop1s()` | `tick20ms()` / `tick1s()` | Same family. |
| `onEnabled(bool)` | `onEnabled(bool)` | **Keep.** This *is* a genuine notification ("you were just enabled"), so the `on` prefix is correct here. |
| `onUpdate(const char*)` | `onControlChanged(const char*)` | It *is* a notification (a control changed), so `on` is right; `onUpdate` is vague — `onControlChanged` says what changed. |
| `applyState()` | `applyState()` (internal) | Core-internal router, not author-facing; low priority to rename. Could become `refresh()` if a verb is wanted, but it isn't in the author's vocabulary. |

**Why this set:** it picks the **prepare / tick / release** spine straight from JUCE (the framework our model already imitates), makes acquire/release *look* like opposites (the #1 R4 complaint), kills the `onBuild*` collision by giving the two concerns visibly different names (`defineControls` vs `prepare`), and applies the industry rule "bare verb = work, `on` = notification" consistently. It reads to a newcomer who has met *any* audio/game framework.

**The honest cost of this set:** it drops the `on…` family feel some may like, and `prepare`/`release`/`tick`/`define` are shorter but *less self-documenting about the light domain* than `onBuildState` (a reader must learn that `prepare` = "build grid-sized state"). And it's the **largest** churn (five hooks, ~140 overrides). Whether that beats Option A (keep + document) is the product owner's call — the survey makes the *case for* renaming stronger than before (the opposites-should-read-as-opposites finding is real), but doesn't make it free.

## The tension, stated fairly

- **For renaming:** the author-facing friendliness the guide is *for*. `onBuildControls`/`onBuildState` are a genuine collision; build/teardown asymmetry is real.
- **Against renaming:** the names are framework-anchored and consistent; 90+ overrides is a lot of churn for a naming preference; and every rename risks *losing* the "build = construct derived state, idempotent" signal the current verb carries. Renaming for brevity can trade a precise name for a vague one.

## Options

**A — Keep the names; sharpen the docs only (the null option).** Leave `onBuildControls`/`onBuildState`/`teardown`, but make the guide + the `///` docs crystal-clear on how the two `onBuild*` differ and that `onBuildState`/`teardown` are the build/release pair. (The guide already does much of this.)
- **Gain:** zero churn, zero risk, keeps the JUCE/UIKit anchor. Solves the *understanding* problem (which is a doc problem) without touching code.
- **Loss:** the surface-level collision remains for someone skimming autocomplete.

**B — Rename only the confusing collision: `onBuildControls` → `onControls` (or `defineControls`).** Keep `onBuildState`/`teardown`; break only the `onBuild*` prefix clash.
- **Gain:** kills the specific "are these the same?" collision (the sharpest part of R4) for the *smaller* blast radius of the two (though `onBuildControls` is the *most*-overridden at 85 — so not actually small). `defineControls` reads as "declare", clearly different from "build state".
- **Loss:** now the two related hooks (`onControls` + `onBuildState`) no longer share a family prefix at all — arguably worse for "these belong together". And 85 overrides is the biggest single rename.

**C — Rename to a symmetric build/release pair.** e.g. `onBuild()` (was `onBuildState`) + `onRelease()` (was `teardown`), and `onControls()` (was `onBuildControls`). Make the acquire/release opposition explicit in the names.
- **Gain:** the strongest answer to *both* halves of R4 — no `onBuild*` collision, and `onBuild`/`onRelease` read as obvious opposites. Cleanest author story.
- **Loss:** the largest churn (all three hooks, ~140 overrides total); drops the recognizable `teardown` name (a widely-understood term — pytest, JUnit, Arduino-adjacent) in favour of a bespoke `onRelease`; and loses the JUCE-anchored `prepareToPlay`-style framing. Trades one recognizable vocabulary for a self-consistent-but-bespoke one — a *Common patterns first* tension.

**D — Rename `onBuildState` → `onBuild`, keep `onBuildControls` and `teardown`.** Minimal: shorten only the most-confused-with-controls one, keep `teardown` (recognizable) and `onBuildControls` (it *is* building the control surface).
- **Gain:** small, targeted; `onBuild` (state) vs `onBuildControls` (controls) still share a prefix (they *are* related) but are now clearly different lengths/words.
- **Loss:** doesn't fully resolve the "read almost identically" concern; `onBuild` alone is vaguer than `onBuildState` about *what* it builds.

**E — Adopt the industry prepare/tick/release spine (the research-grounded set above).** `defineControls` / `prepare` / `tick`(+`tick20ms`/`tick1s`) / `release`, keep `onEnabled`, rename `onUpdate`→`onControlChanged`. This is the option the survey points to.
- **Gain:** the *strongest* alignment with recognizable frameworks (JUCE prepare/process/release is our exact model). Fixes every R4 complaint at once: acquire/release read as opposites (`prepare`↔`release`), the `onBuild*` collision is gone (`defineControls` vs `prepare` look as different as they are), and the "bare verb = work, `on` = notification" rule is applied consistently. A newcomer from *any* audio/game framework recognizes it.
- **Loss:** the largest churn (~140 overrides across all modules); the light-domain names get *shorter but less self-documenting* (`prepare` needs learning that it means "build grid-sized state," where `onBuildState` half-explained itself); and `release`/`tick`/`prepare` are generic (a grep for `prepare` hits more noise than `onBuildState`).

## Recommendation (for the product owner to accept or override)

**Chosen: E.** The survey turned the core R4 complaint from "preference" into an objective gap — *every* surveyed framework names acquire/release as visible opposites, and ours (`onBuildState`↔`teardown`) don't — and pointed to the exact vocabulary (`prepare`/`tick`/`release`) that the runtime, the codebase, and the industry already share. For a from-scratch project explicitly aiming for zero tech debt, adopting the best-practice names now (a one-time ~140-override sweep) beats carrying a mixed vocabulary forward. Option A (keep + document) was the alternative — safe, zero-churn, understanding-via-docs — and B/C/D were rejected as half-measures (B fixes only the collision, C invents a bespoke `onRelease` no framework uses, D is cosmetic).

## What stays, and why (`setup()` — the Arduino-semantics question)

`setup()` **keeps its name** and is *not* replaced by `prepare()` — because they are two genuinely different phases that Arduino's single `setup()` conflates:

- **projectMM `setup()`** = **one-time, enabled-independent wiring**, run **once** in a module's life and never repeated (e.g. NetworkModule pushing the DHCP hostname once before any bring-up). It ignores enable/disable and is not re-run on config change.
- **`prepare()` (was `onBuildState`)** = **build derived state for the *current* config**, re-run **every time** the grid/config changes, gated by effective-enabled.

So the semantics differ from Arduino's catch-all `setup()` *and* from `prepare()`: `setup` is "once, forever"; `prepare` is "whenever the config changes." They coexist. `setup()` keeps the name because it's *already* the industry-standard term for one-time init — JUnit `setUp`, pytest `setup_method`, xUnit `Setup`, Arduino `setup` — so it needs no change to be best-practice. It's simply **invisible to effect authors** (no effect needs enabled-independent one-time wiring), which is why the guide drops it from the author's view while it lives on for core modules.

## Applies to ALL modules, not just catalog modules

This vocabulary is defined **once on `MoonModule`** and inherited by every module — catalog (effects/layouts/modifiers/drivers) *and* core (Network, Http, Filesystem, System, …). The rename touches all of them, and **that uniformity is the point**: a core module and a light effect override the *same-named* hooks, so a contributor who learns the lifecycle in the guide reads core code fluently too. (Pragmatic per-domain differences like header umbrellas or the `.h`+`.cpp` split stay as they are — but the *hook names* are universal.)

## Execution notes (the mechanical rename)

- One hook per commit, mechanical: rename the base virtual, all overrides across `src/` (core + light), the `applyState`/`Scheduler` call sites, then regenerate moxygen docs and update the guide + architecture + coding-standards.
- `check_specs` + the full test suite are the safety net (a missed call site fails to compile; a missed doc reference fails the spec check).
- Suggested order: `loop`→`tick` first (self-contained, no acquire/release entanglement), then `teardown`→`release` (coordinate with the [ScratchBuffer plan](Plan-20260710%20-%20Scratch%20buffer%20helper%20for%20memory-holding%20effects%20(shipped).md)'s private-`release` removal so the two `release`s don't clash), then `onBuildState`→`prepare`, then `onBuildControls`→`defineControls`, then `onUpdate`→`onControlChanged`. Each is independently shippable and behaviour-neutral.
- Update the guide's hook table + examples to the new names in the same sweep, including the one-line beginner aside for `tick()` ("runs once per frame, like a tick of a clock — do a little each time, don't write your own loop").

## Scope guard

Behaviour-neutral naming change only — no lifecycle logic changes. Do it as a series of mechanical per-hook commits (order above). The names apply uniformly to core and catalog modules; `setup()`, `onEnabled()`, and `applyState()` keep their names for the reasons stated.
