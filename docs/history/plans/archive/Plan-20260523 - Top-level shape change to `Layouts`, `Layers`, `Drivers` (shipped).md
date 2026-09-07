# Plan: Top-level shape change to `Layouts`, `Layers`, `Drivers`

## Goal

Rename and re-shape the three light-domain top-level containers from singletons-of-things to plural containers-of-things, so the side-nav reads honestly:

```text
Layouts             ← was LayoutGroup
  ├─ GridLayout
  └─ (room for more)
Layers              ← NEW (today there's only one Layer at root)
  └─ Layer
       ├─ NoiseEffect
       ├─ MirrorModifier
       └─ (effects + modifiers)
Drivers             ← was DriverGroup
  ├─ ArtNetSendDriver
  └─ PreviewDriver
```

Each container is a regular `MoonModule` with a `Generic` role. The shape change is the deliverable; the **blend/composition of multiple Layers** and the **per-Layer start/end carving** are tracked as follow-ups but **the `start/end` controls land in this commit** so the surface is stable when composition arrives.

## Scope decisions confirmed

- **Q1 — composition (a):** Drivers will eventually compose N Layer buffers into a single output (alpha-blend or additive). Already documented in [architecture-light.md:123,137](docs/architecture-light.md) and [DriverGroup.md:20](docs/moonmodules/light/DriverGroup.md). **Follow-up; not in this commit.** With one Layer the compose step is a copy.
- **Q2 — Layouts shared (d) + per-Layer ranges (f):** All Layers share the same `Layouts` instance (today's model — [architecture-light.md:53](docs/architecture-light.md#L53)). Each Layer carries `startX/Y/Z` and `endX/Y/Z` controls that select a region of the shared layout. Defaults: whole layout. With one Layer the controls are no-ops; with N Layers + composition the carving becomes active.
- **Q3 — (g) rename and keep as containers:** all three top-level containers are concrete `MoonModule` subclasses with `Generic` role. Reject (h) (flat top level) and (i) (templated `RoleContainer<T>`). Each container *does* hold real state (LayoutGroup stitches indices, DriverGroup owns the output buffer; future `Layers` will own the composed buffer).
- **Q4 — ship shape only:** rename, introduce `Layers`, add `start/end` to `Layer`. Multi-Layer composition and the live carving of layout regions are a separate commit.

## Mapping: old → new

| Today | New | What it is |
|---|---|---|
| `class LayoutGroup` in [src/light/layouts/LayoutGroup.h](src/light/layouts/LayoutGroup.h) | `class Layouts` (same file, class renamed) | Holds N `LayoutBase` children, stitches indices via `forEachCoord`. **Behaviour unchanged.** |
| `class DriverGroup` in [src/light/drivers/DriverGroup.h](src/light/drivers/DriverGroup.h) | `class Drivers` (same file, class renamed) | Holds N `DriverBase` children, owns the LUT-blended output buffer, hands the buffer pointer to each driver. **Behaviour unchanged.** Still reads from a single `Layer*` (composition is the follow-up). |
| `Layer` at the root | `class Layers` (new file [src/light/Layers.h](src/light/Layers.h)) **wraps** N `Layer` children | New container. `loop()` runs each child Layer in order. With one child Layer it's a thin pass-through (same behaviour as today). |
| `Layer::startX/Y/Z`, `endX/Y/Z` | new controls on `Layer` | Default to `(0,0,0)`–`(physW-1, physH-1, physD-1)` (i.e. whole layout). Today no-op; persisted and visible in the UI for the composition follow-up. |

The factory string keys also rename:
- `"LayoutGroup"` → `"Layouts"`
- `"DriverGroup"` → `"Drivers"`
- new key `"Layers"` (the container)
- existing `"Layer"` unchanged (the child class)

This breaks any persisted `/.config/*.json` from earlier sessions that reference the old `"LayoutGroup"` / `"DriverGroup"` type names — see "Migration" below.

## Files to change

### Renames (1 class rename per file; preserve git history via in-place edit)

- **[src/light/layouts/LayoutGroup.h](src/light/layouts/LayoutGroup.h)** — rename `class LayoutGroup` → `class Layouts`. Filename **stays** `LayoutGroup.h` so this is a class rename in place; everywhere that includes `light/layouts/LayoutGroup.h` keeps working. Update the `#include` comment in [src/light/Layer.h](src/light/Layer.h) and add an alias if needed.
  - *Alternative:* rename file too (`LayoutGroup.h` → `Layouts.h`) via `git mv`. Cleaner long-term but breaks every `#include`. **Plan picks file rename via `git mv`** because we just did a folder restructure last commit; one more rename is consistent with the cleanup.
- **[src/light/drivers/DriverGroup.h](src/light/drivers/DriverGroup.h)** → `src/light/drivers/Drivers.h`, `class DriverGroup` → `class Drivers`. Same treatment.

### New file

- **[src/light/Layers.h](src/light/Layers.h)** — new container class. Roughly:
  ```cpp
  #pragma once
  #include "core/MoonModule.h"
  #include "light/Layer.h"

  namespace mm {

  // Top-level container for one or more Layers. Each child Layer reads its
  // buffer from a shared Layouts instance and writes its own buffer; Drivers
  // composes them on the output side (composition follow-up).
  //
  // With one child Layer today this is a thin pass-through: loop() runs the
  // child Layer's loop() in order. The container itself owns no buffer.
  class Layers : public MoonModule {
  public:
      void setLayouts(Layouts* l) {
          layouts_ = l;
          // Propagate to all child Layers so they can size their buffers.
          for (uint8_t i = 0; i < childCount(); i++) {
              if (auto* lyr = dynamic_cast<Layer*>(child(i))) {
                  lyr->setLayoutGroup(layouts_);  // method name unchanged
              }
          }
      }

      Layouts* layouts() const { return layouts_; }

      void loop() override {
          // Scheduler gates Layers itself by respectsEnabled() default.
          for (uint8_t i = 0; i < childCount(); i++) {
              if (!child(i)->enabled()) continue;
              uint32_t start = platform::micros();
              child(i)->loop();
              child(i)->addAccumUs(platform::micros() - start);
          }
      }

      // Active Layer for Drivers' single-Layer plumbing (placeholder until
      // composition lands). Returns the first child Layer, or nullptr.
      Layer* activeLayer() const {
          for (uint8_t i = 0; i < childCount(); i++) {
              if (auto* lyr = dynamic_cast<Layer*>(child(i))) return lyr;
          }
          return nullptr;
      }

  private:
      Layouts* layouts_ = nullptr;
  };

  } // namespace mm
  ```
  - **Hot-path note:** `dynamic_cast` is in the cold path only (`setLayouts` runs at startup, `activeLayer` at composition setup). Per-frame `loop()` uses `child(i)->loop()` — no cast. No RTTI cost in the render path.
  - **Alternative without dynamic_cast:** since every child of `Layers` is by construction a `Layer`, `static_cast` is safe. Use that — matches the existing pattern in `Layouts::forEachCoord` (`static_cast<LayoutBase*>(child(i))`). **Plan adopts `static_cast`.**

### Layer changes

- **[src/light/Layer.h](src/light/Layer.h)** —
  - Add `lengthType startX_ = 0, startY_ = 0, startZ_ = 0` and `lengthType endX_ = -1, endY_ = -1, endZ_ = -1` members. `-1` means "use full layout extent."
  - `onBuildControls()` adds these as `controls_.addInt16("startX", startX_, 0, physW)` etc. (uses int16 control if available; if only uint8 exists today, add uint16 controls — see "Controls" below).
  - In `rebuildLUT()` / `onAllocateMemory()`, when computing `width_/height_/depth_` from the layout, **honour the start/end fields** if they're non-default. With one Layer and defaults, the result is identical to today.
  - Update `setLayoutGroup(LayoutGroup*)` to accept `Layouts*` (just a type rename — same pointer semantics). Keep the method name `setLayoutGroup` for one cycle, or rename to `setLayouts`. **Plan picks `setLayouts`** since we're renaming everything else anyway, and the inconsistency would be confusing.

### main.cpp wiring

Old:

```cpp
auto* layoutGroup = create("LayoutGroup");
auto* grid = create("GridLayout"); layoutGroup->addChild(grid);
auto* layer = create("Layer"); layer->setLayoutGroup(layoutGroup);
layer->addChild(create("NoiseEffect"));
layer->addChild(create("MirrorModifier"));
auto* driverGroup = create("DriverGroup"); driverGroup->setLayer(layer);
driverGroup->addChild(create("ArtNetSendDriver"));
driverGroup->addChild(create("PreviewDriver"));

scheduler.addModule(layoutGroup);
scheduler.addModule(layer);
scheduler.addModule(driverGroup);
```

New:

```cpp
auto* layouts = create("Layouts");
auto* grid = create("GridLayout"); layouts->addChild(grid);

auto* layersContainer = create("Layers");
static_cast<Layers*>(layersContainer)->setLayouts(static_cast<Layouts*>(layouts));
auto* layer = create("Layer");
layersContainer->addChild(layer);
static_cast<Layer*>(layer)->setLayouts(static_cast<Layouts*>(layouts));  // happens via setLayouts too
layer->addChild(create("NoiseEffect"));
layer->addChild(create("MirrorModifier"));

auto* drivers = create("Drivers");
static_cast<Drivers*>(drivers)->setLayer(static_cast<Layer*>(layer));  // placeholder; composition follow-up will read from Layers
drivers->addChild(create("ArtNetSendDriver"));
drivers->addChild(create("PreviewDriver"));

scheduler.addModule(layouts);
scheduler.addModule(layersContainer);
scheduler.addModule(drivers);
```

### Factory + display name

- [src/main.cpp](src/main.cpp) `registerModuleTypes()`:
  - `registerType<Layouts>("Layouts", "light/Layouts.md")`
  - `registerType<Layer>("Layer", "light/Layer.md")` — unchanged
  - `registerType<class mm::Layers>("Layers", "light/Layers.md")` — new
  - `registerType<Drivers>("Drivers", "light/drivers/Drivers.md")`
- `ModuleFactory::displayNameFor` strips role-noun suffixes (`Effect`/`Modifier`/etc.). The new names `Layouts`, `Layers`, `Drivers` don't end with any of those, so they pass through unchanged — UI shows them as written. ✓

### UI side (none, mostly)

The UI is module-driven — it renders whatever the tree says. `acceptsChildren` in [src/ui/app.js](src/ui/app.js) currently allows Effect+Modifier in Layer, Driver in DriverGroup, Layout in LayoutGroup. Update:
- `acceptsChildren` mapping: `"Layouts"` accepts Layout role; `"Layers"` accepts a single role (`Layer`) — but `Layer` isn't a role, it's a *concrete type*. So either (a) introduce a `Layer` role distinct from Generic, or (b) keep `Layers` accepting type-name `Layer` as a special case, or (c) have `Layers` accept Generic children. **Plan picks (a) — add `ModuleRole::Layer`** to the enum. It's a small change, makes the role chip emit `🚇` (or another emoji — the UI's `ROLE_EMOJI` map gains one entry), and the type picker filters correctly.

Wait — that adds noise. Let me reconsider:

  - **(b) is the lightest:** `acceptsChildren` for `"Layers"` is hardcoded to `[Layer]` (by typeName, not role). The UI already special-cases this kind of containment via `acceptsChildren`. The role chip on each `Layer` card stays Generic (⚙️). Slightly cluttered emoji-wise but no role-enum change.
  - **(a) is cleaner long-term:** add `ModuleRole::Layer` to the enum. The UI ROLE_EMOJI map gets a new entry (need to pick an emoji — 🪟 / 🎞️ / 🧱 are candidates, will ask the product owner). [check_specs.py](moondeck/check/check_specs.py) might depend on the role list; verify.

  **Plan picks (a)** because we're already changing the shape and adding a role is cheaper than a special-case in the UI. **One emoji to pick during implementation.**

### Spec updates

- **[docs/moonmodules/light/Layer.md](../../../moonmodules/light/moxygen/Layer.md)** — update intro to "renders into a buffer sized by either the full Layouts extent or a carved region (start/end controls)." Document the new `setLayouts` method.
- **[docs/moonmodules/light/Layouts.md](../../../moonmodules/light/moxygen/Layouts.md)** — rename from `LayoutGroup.md`; class is `Layouts`. Body mostly unchanged (still describes the index-stitching).
- **[docs/moonmodules/light/Layers.md](../../../moonmodules/light/moxygen/Layers.md)** — NEW. Describes the container: holds N Layers, runs each in order in `loop()`, future home of the composed-buffer logic. Single-line forward-reference to the composition follow-up.
- **[docs/moonmodules/light/drivers/Drivers.md](../../../moonmodules/light/moxygen/Drivers.md)** — rename from `DriverGroup.md`; class is `Drivers`.
- **[docs/architecture-light.md](docs/architecture-light.md)** — update the pipeline diagram and any prose that names `LayoutGroup`/`DriverGroup`/singular `Layer`. The "UI integration (light domain)" tree shape gets `Layouts → Layers → Drivers` at the top level.
- **[docs/moonmodules/light/EffectBase.md](../../../moonmodules/light/moxygen/EffectBase.md)** — passing reference: parent is still `Layer`, no change.
- **[docs/plan.md](docs/plan.md)** — add a `Multi-Layer composition (pending)` entry covering (a) compose, (b) per-Layer start/end carving activation.
- **[README.md](README.md)** — scan for module type names; update if any examples use `LayoutGroup`/`DriverGroup`.

### Tests

- **[test/test_grid_layout.cpp](test/test_grid_layout.cpp)** — references `LayoutGroup`; rename to `Layouts`.
- **Other tests using `LayoutGroup`/`DriverGroup`** — same. Likely test_extrude, test_mirror, test_preview_driver, scenarios. `grep -rln "LayoutGroup\|DriverGroup"` will find all.
- **New test: `test_layers_container.cpp`** —
  - One Layers container with one Layer + one effect (RainbowEffect): produces same byte-for-byte buffer as the old single-Layer model.
  - One Layers container with two Layers (each with one effect): both child loops run, both buffers are populated. Composition not tested (follow-up).
- **Scenarios** — [test/scenarios/*.json](test/scenarios) reference `LayoutGroup`/`DriverGroup` by type-name strings. Update each. Behaviour byte-identical with one Layer.

### Migration (persisted config)

[src/core/FilesystemModule.h](src/core/FilesystemModule.h) writes per-module JSON keyed by **typeName** (e.g. `/.config/LayoutGroup.json`). After rename:
- Either delete the old `.config/*.json` files at boot (easy but loses control values), or
- Add a one-time migration map in `FilesystemModule::load` (`LayoutGroup → Layouts`, etc.).

**Plan picks: delete-and-warn.** On first boot after this commit, if `.config/LayoutGroup.json` exists, log a warning and delete it. Same for `DriverGroup.json`. The user's control values for these containers were near-zero (no per-instance controls today besides `enabled`), so loss is minimal. Saves implementing a migration framework for one commit.

## Implementation order

1. **Add `ModuleRole::Layer`** to [src/core/MoonModule.h](src/core/MoonModule.h). Update `roleName()`. Verify [moondeck/check/check_specs.py](moondeck/check/check_specs.py) doesn't have a hardcoded role list. Build to check for warnings.
2. **Rename `LayoutGroup` → `Layouts`** (class + file via `git mv` + factory key). Update all `#include`s, all `static_cast<LayoutGroup*>`, all references in tests + scenarios + spec. Build + run all tests; expect green (no behaviour change).
3. **Rename `DriverGroup` → `Drivers`** (same treatment).
4. **Add `class Layers`** in [src/light/Layers.h](src/light/Layers.h). Add `setLayouts()` to `Layer`. main.cpp creates `Layers` containing one `Layer`. Run all tests; live-verify with a desktop run that the pipeline still produces frames.
5. **Add `start/end` controls to `Layer`** — uint16 (or int16 if available) with sensible bounds. Default = whole layout. `rebuildLUT()` honours them when not at default. Update `test_layer*.cpp` and add a test asserting "Layer with default start/end matches old Layer behaviour byte-for-byte."
6. **UI emoji pick** for `ModuleRole::Layer` — ask the product owner. Add to `ROLE_EMOJI` map in [src/ui/app.js](src/ui/app.js).
7. **Update specs** ([Layer.md](../../../moonmodules/light/moxygen/Layer.md), new [Layouts.md](../../../moonmodules/light/moxygen/Layouts.md), new [Layers.md](../../../moonmodules/light/moxygen/Layers.md), new [Drivers.md](../../../moonmodules/light/moxygen/Drivers.md), [architecture-light.md](docs/architecture-light.md), [plan.md](docs/plan.md), [README.md](README.md) if needed). Run [check_specs.py](moondeck/check/check_specs.py).
8. **Migration**: FilesystemModule deletes `.config/LayoutGroup.json` and `.config/DriverGroup.json` if present, logs a warning.
9. **All pre-commit gates 1–6** (build, ctest, scenarios, platform boundary, specs, ESP32). Reviewer agent (gate 7) after.

## Verification checklist

- [ ] `cmake --build build` — zero warnings, builds clean.
- [ ] `ctest` — all unit tests pass, including the new `test_layers_container.cpp` cases.
- [ ] `./build/test/mm_scenarios` — all scenarios pass (after their `LayoutGroup`/`DriverGroup` type-name updates).
- [ ] [check_platform_boundary.py](moondeck/check/check_platform_boundary.py) — PASS.
- [ ] [check_specs.py](moondeck/check/check_specs.py) — all specs ok.
- [ ] [build_esp32.py](moondeck/build/build_esp32.py) — clean ESP32 build.
- [ ] Live desktop run: `/api/types` shows `Layouts`, `Layers`, `Drivers` (no longer `LayoutGroup`, `DriverGroup`). `/api/state` shows the new tree shape. Effects render correctly through the new wiring. Tick time within run-to-run jitter of the previous commit.
- [ ] UI side-nav reads `Layouts`, `Layers`, `Drivers`. Cards under `Layers` contain one `Layer` with effects+modifiers inside. Drag-reorder still works within each container.
- [ ] One snapshot ESP32 run verifies no regression — same scenario, same FPS within jitter.
- [ ] Reviewer agent (Opus) — PASS.

## Open variations / decisions during implementation

- **Emoji for `ModuleRole::Layer`** — product owner picks. Suggestions: 🪟 (layered glass), 🎞️ (film strip = sequential layers), 🧱 (brick = stacked).
- **Control type for `start/end`** — if uint8 only, range is 0–255 (fine for current grids up to 128). If int16/uint16 is available, use that. Check existing `Control` types — there's already a `Uint16` (used by `httpServer->port`).
- **Layout file rename or class-only rename?** Plan picks `git mv` for `Layouts.h` and `Drivers.h`. Reject if it makes the diff harder to review — fall back to class-rename-in-place.

## Notes for the implementer

- This is a **shape change with explicit no-behaviour-change goal** (composition is the follow-up). Every test should pass with byte-identical output to the previous commit, modulo the type-name strings in JSON config and scenarios.
- The new `Layers` container is **not** the right place to put extrude logic, blend logic, or buffer ownership today. Those stay on `Layer` (and on `Drivers`'s output buffer). Resist the temptation to "while we're here, also…" — that's the projectMM-priority bloat trap.
- `dynamic_cast` is disabled on ESP32 (RTTI off). Use `static_cast<Layer*>(child(i))` everywhere — same pattern as `Layouts::forEachCoord` does for `LayoutBase`.
- Per CLAUDE.md: this needs to be planned (this file), implemented in a feature branch (we're on `next-iteration`), tested, then product-owner-approved before commit. Pre-commit gates 1–6 are not optional. The reviewer agent must PASS.
- The plan should be saved as `docs/history/plan-NN.md` after implementation per CLAUDE.md's per-feature workflow. Numbering picks up from the latest in `docs/history/` (not the archived ones).
