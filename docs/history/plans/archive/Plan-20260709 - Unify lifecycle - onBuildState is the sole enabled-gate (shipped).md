# Plan — Unify the module lifecycle: `onBuildState` is the sole enabled-gate

> **As implemented (3e37987) — the design evolved during build.** This plan proposed "Option B": `onBuildState()` stays the single hook and *builds the empty state* (releases everything) when `!effectivelyEnabled()`, with `buildState()` always calling it on every node (decisions #1 and #3 below). During implementation that was sharpened one step further into a cleaner central router: **`MoonModule::applyState()`** is the sole orchestration point — it calls `onBuildState()` (a pure *build*, no `enabled()` check) on an effectively-enabled node and **`teardown()`** (release) on a disabled one, recursing the tree. So `onBuildState()` is NOT the "sole gate" the title says and does NOT build-empty-when-disabled; the *release* lives in `teardown()`, and `applyState()` — not the caller — decides which runs. The Scheduler's boot Phase-4 sweep and `buildState()` call `applyState()` (not `onBuildState()` directly). Everything else below (effective-enabled cascade #2, CLASS-1 vs CLASS-2, the goal of zero per-module `enabled()` gates) shipped as written. Kept as the intent record; read the code + [lessons.md](../../lessons.md) for the final shape.

## Context

The [disable-releases-resources commit](Plan-20260709%20-%20Disabling%20releases%20resources%20(onEnabled%20per%20module)%20(shipped,%20superseded).md) left the same idea — "a disabled module holds no resources" — expressed in **two mechanisms** across ~7 modules, plus ~24 self-`enabled()` gates scattered through `setup()`/`onBuildState()`/`onCorrectionChanged()`/setters. The product owner's read (correct, verified in code): this is **sharpening, not a rewrite** — the orchestration already exists and runs; 10+ effects already release-on-disable through `onBuildState`; the delete cascade is already correct. (The full design study that fed this plan, `docs/backlog/lifecycle-unification-analysis.md`, was retired once the work shipped — its analysis is folded into this plan and [lessons.md](../../lessons.md).)

**Decisions (product owner, this session):**
1. **Option B** — `onBuildState()` is the single "(re)build my derived state for my current (controls, enabled)" entry point, and **`enabled==false` builds the *empty* state (release everything — memory AND hardware)**. Retire `onEnabled` for resource acquire/release.
2. **Cascade = effective-enabled** — the release gate tests "am I OR any ancestor disabled", not the raw local flag. Walk `parent_`. (Not flag-cascade — keeps each module's own persisted `enabled` honest.)
3. **Sweep = always-call, build-empty-when-disabled** — `buildState()` still calls `onBuildState()` on every node; each builds its empty state when effectively-disabled. (Not central-skip, which would reintroduce a transition-time release step.)

**Goal:** catalog modules get leaner — the 24 CLASS-1 self-gates vanish (a driver's `setup()` becomes `{ parseConfig(); reinit(); }`), and *all* enabled-orchestration lives in core. The 12 CLASS-2 uses (a parent composing from its children's `enabled()`) are legitimate domain logic and **stay untouched**.

## Verified current state (code, file:line)

- **The disable toggle already runs the release path.** `Scheduler::setControl` "enabled" branch: `setEnabled(...)` then `buildState()` → `onBuildState()` on every module. ([Scheduler.cpp:218-223](../../../src/core/Scheduler.cpp#L218))
- **Boot:** Phase 3 `setup()` + Phase 4 `onBuildState()`, both ungated. ([Scheduler.cpp:48-58](../../../src/core/Scheduler.cpp#L48))
- **`buildState()` visits every node directly** (roots + base recursion) — does NOT skip a disabled parent's subtree. ([Scheduler.cpp:140-144](../../../src/core/Scheduler.cpp#L140)) — this is why cascade needs effective-enabled.
- **Effects already do B** via `onBuildState`: GameOfLife/GEQ/Fire/Tetrix/… `if (enabled() && n>0) alloc; else release()`.
- **`parent_` exists** with `parent()`/`setParent()`, set by `addChild`. ([MoonModule.h:246,338,468](../../../src/core/MoonModule.h#L246)) → effective-enabled can walk ancestors.
- **`respectsEnabled()==false`** = "always runs" (Network/HttpServer/Filesystem), NOT "forces children on". The walk must treat an opted-out ancestor as neutral, not as a disable. ([MoonModule.h:226-230](../../../src/core/MoonModule.h#L226))
- **`onEnabled` overrides to retire:** RmtLed, ParallelLed, NetworkSend, AudioService, DevicesModule, IrService, NetworkReceiveEffect — all route to setup/teardown (pure resource acquire/release). ([grep `void onEnabled`])
- **The ONE edge-trigger to keep:** `MqttModule::onEnabled` does a **clean protocol DISCONNECT** (a courtesy MQTT frame + backoff reset), not just a socket close — and its *connect* is lazy on `loop1s()`, not in `onBuildState`. This is a genuine edge-triggered one-shot, not "build derived state". ([MqttModule.h:82](../../../src/core/MqttModule.h#L82)) It stays on `onEnabled`; the plan sharpens the contract to say *that* is what `onEnabled` is for.
- **Delete cascade correct:** both delete paths run `removeChild → teardown() → deleteTree → buildState()`; `teardown()` recurses (reverse), `deleteTree` recurses. ([HttpServerModule.cpp:1518-1525](../../../src/core/HttpServerModule.cpp#L1518), [applyClearChildren:1439-1441](../../../src/core/HttpServerModule.cpp#L1439))

## Design

### 1. `MoonModule::effectivelyEnabled()` — the cascade predicate (core)
```cpp
/// True unless this module OR an ancestor that respects the enabled flag is disabled.
/// A respectsEnabled()==false ancestor is neutral (always-on, does not force children).
bool effectivelyEnabled() const {
    for (const MoonModule* m = this; m; m = m->parent())
        if (m->respectsEnabled() && !m->enabled()) return false;
    return true;
}
```
One recognizable construct (an inherited/computed property — the same shape CSS `visibility`, a DOM `disabled` cascade, or a scene-graph `worldVisible` use). Off the hot path (called from `onBuildState`, not `loop`). This is the single new primitive; it earns its place by being the thing every resource-holder leans on.

### 2. The contract, written once on the base `onBuildState` doc (core)
Sharpen [MoonModule.h onBuildState](../../../src/core/MoonModule.h) doc: *"Build this module's derived state (buffers, peripherals, LUTs) for its current controls and `effectivelyEnabled()`. When `!effectivelyEnabled()`, build the **empty** state — release every buffer and peripheral this module holds. Idempotent and cheap when nothing changed (guard re-acquire with a 'already sized' check). Runs on boot, on any dims/mapping control change, and on every enable/disable toggle."* Also sharpen the `onEnabled` doc: *"For genuine edge-triggered one-shots that are NOT 'rebuild derived state' — e.g. a clean protocol disconnect. Resource acquire/release belongs in `onBuildState`, not here."*

### 3. Each resource-holder: fold release into `onBuildState`'s disabled branch, drop `onEnabled` (light + core)
Per module (RmtLed, ParallelLed, AudioService, DevicesModule, IrService, NetworkSendDriver, NetworkReceiveEffect):
- `onBuildState()`: `if (!effectivelyEnabled()) { <release everything>; return the-empty-build; }  else { <acquire/build> }`. Drivers already have `reinit()` here — change the gate from `enabled()` to `effectivelyEnabled()` and make the else-branch call the existing `teardown()`/release. AudioService's `active_` election + DevicesModule's `active_` seat move into the enabled branch (claim) / disabled branch (vacate).
- **Delete** the `onEnabled` override (except MqttModule).
- **Delete** the boot `setup()` self-gate — `setup()` returns to enabled-independent one-time wiring only (controls already bound in Phase 1). Verify no acquire remains in any `setup()`.
- `teardown()` stays (destruction + explicit removal path) and may delegate to the same release helper the disabled branch uses — "disable == teardown, module stays alive" is then literally true.

### 4. Effects: swap `enabled()` → `effectivelyEnabled()` (light)
The 10+ effects already gate `onBuildState` on `enabled()`. Change to `effectivelyEnabled()` so a disabled *parent Layer* releases its effects' heap too (today a disabled Layer's loop is skipped, but its effects' `onBuildState` still sees their own `enabled()==true` and keeps memory). Mechanical, one token per site.

### 5. CLASS-2 stays untouched (light)
`Layer` (fold enabled modifiers), `Layers` (pick active layer), `Layouts` (sum enabled children) read *children's* `enabled()` to compose the pipeline. Leave exactly as-is — legitimate domain logic, not self-orchestration. The plan explicitly does NOT touch these 12 sites. (If a parent is disabled, its `loop()` is already skipped structurally, so composition never runs anyway.)

### 6. Does core need to gate `setup()`/`onBuildState()` at the Scheduler? 
No — decision 3 is always-call. `buildState()` keeps calling every node; the `effectivelyEnabled()` check inside each `onBuildState()` is the gate. The boot Phase-3 `setup()` no longer acquires (§3), so it needs no gate either. **Net: the Scheduler loses zero code and gains zero gates** — all the gating collapses into `onBuildState` + `effectivelyEnabled()`. This is the leanest shape and keeps the orchestrator untouched.

## Files

- **Core:** `MoonModule.h` (`effectivelyEnabled()` + the two doc contracts), `AudioService.h`, `DevicesModule.h`, `IrService.h` (fold release into onBuildState, drop onEnabled). `MqttModule.h` — **no change** (its onEnabled is the sanctioned edge-trigger; add a one-line doc noting why it's exempt).
- **Light:** `RmtLedDriver.h`, `ParallelLedDriver.h`, `NetworkSendDriver.h`, `NetworkReceiveEffect.h` (fold + drop onEnabled + remove setup gate); the ~10 effects (`enabled()`→`effectivelyEnabled()` in onBuildState). `DriverBase.h` — `releaseOnDisable` helper is no longer called from onEnabled; either repurpose it as the release helper onBuildState's disabled branch calls, or delete if each driver's teardown suffices (decide during impl — subtraction preferred).
- **Tests:** extend the three existing boot-gate tests to assert via `effectivelyEnabled`; **add cascade tests** (§ Verification).
- **Docs:** `architecture.md` (the lifecycle section — one gate, the contract), `lessons.md` (fold the arc: the three traps collapse into "onBuildState is the release gate, keyed on effective-enabled"). Retire the now-shipped study (`backlog/lifecycle-unification-analysis.md`) and the [backlog entry](../../../backlog/backlog-core.md) per *Mandatory subtraction*. Mark this plan `(shipped)`.

## Verification

1. `cmake --build build` clean (0 warn); `ctest`; scenarios; `check_specs`; platform boundary; ESP32 P4 build.
2. **Existing regression tests still pass** (they assert behaviour: disabled → released) — the safety net that this refactor preserves the contract.
3. **New cascade tests (mandatory):**
   - Disable a parent container → every descendant releases (assert a driver's buffer freed AND an effect's heap freed under it).
   - Re-enable the parent → every descendant that is *itself* enabled re-acquires; one left individually-disabled stays released.
   - A `respectsEnabled()==false` ancestor does not force a disabled child on (effective-enabled treats it neutrally).
   - Delete a parent → whole subtree torn down + freed (regression-guard the already-correct path).
4. **Leanness check:** grep confirms the 24 CLASS-1 self-gates are gone; the 12 CLASS-2 remain; no `onEnabled` resource-release overrides remain except MqttModule.
5. **Hardware:** on the P4 (.133), the RmtLed(disabled)/ParlioLed(enabled) shared-pin-20 case still cold-boots clean; disable ParlioLed's parent Drivers container (if togglable) or the Layer → strip goes dark and memory frees; re-enable → re-acquires. No reboot.

## Scope guard

Sharpening, not a rewrite. Touch only the ~7 resource-holders + the ~10 effects' gate token + the one core primitive. **Do NOT** touch CLASS-2 (Layer/Layers/Layouts child composition). **Do NOT** add a third lifecycle hook (Option C rejected). **Do NOT** gate at the Scheduler (always-call). MqttModule's onEnabled is the one sanctioned edge-trigger and stays. Keep `respectsEnabled()` exactly as-is.
