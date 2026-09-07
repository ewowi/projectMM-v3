# Plan — TasksModule (nested RTOS-task → module view), Phase 1 read-only

## Context

projectMM's architecture already commits (🚧, not yet built) to per-module core affinity: "each MoonModule can declare a core affinity; the scheduler respects this when pinning tasks" ([architecture.md § Parallelism](../../../architecture.md#parallelism)), and the backlog holds *Task core-pinning* and a *core-1 driver task*. None of the *optimization* exists yet — and you can't optimize what you can't see. This module is the **observability foundation**: show every FreeRTOS task and the projectMM modules that run inside each, with cost. Inspired by MoonLight's [`ModuleTasks`](https://github.com/MoonModules/MoonLight/blob/main/src/MoonBase/Modules/ModuleTasks.h) (a flat task table); projectMM's version nests modules under their task and leans on projectMM's *already-free* per-module self-report.

Critical framing + the System-Modules taxonomy this fits into: [docs/backlog/system-modules.md](../../../backlog/system-modules.md). (The original pre-implementation spec draft was deleted once the module shipped — its final spec is [core/system.md § Tasks](../../../moonmodules/core/system.md#tasks) + the `TasksModule.h` `///`.)

## Decisions locked (PO)

- **Phase 1 = read-only** nested view. Relocate + multi-task scheduler = Phase 2 (separate effort, needs the 🚧 core-affinity mechanism).
- **Build agile in steps** toward the nested tree (1 → 4 below); commit grouping decided at plan review.
- **Cost tiering (measured on the bench):** MoonModule cost view = free (already collected); RTOS task list = cheap (`USE_TRACE_FACILITY`, no per-tick cost) → ships on; per-task **CPU%** = ~5 % tick (`GENERATE_RUN_TIME_STATS`) → **build-flag `MM_TASK_CPU_STATS`, off by default**.
- **Doc home:** a section in `docs/moonmodules/core/system.md` (with the other fixed System modules); technical page auto-generated from `///`.
- **Bespoke acknowledged:** the task→modules nesting is projectMM-specific (a task normally *is* the unit); the reason is stated at the introduction site. "Relocate" is genuinely novel and deferred.

## Design

`TasksModule : public MoonModule, public ListSource` — the exact shape of `DevicesModule` / `I2cScanModule` (read-only discovery via `ControlType::List` + the `ListSource` adapter, the UITableView/`QAbstractItemModel` data-source pattern architecture.md blesses). `.h` + `.cpp` per the core-module convention. Registered in `main.cpp` with docPath `core/system.md#tasks`. Refreshes on `loop1s()` (not hot-path).

### Step 1 — MoonModule cost table (zero cost, no FreeRTOS, cross-platform)

- The module is a `ListSource`; `addList("modules", *this)` in `onBuildControls`.
- `listRowCount()` = the Scheduler's module count; `writeListRow(sink, i)` emits `{name, us, class, heap}` straight from `Scheduler::module(i)->{loopTimeUs(), classSize(), dynamicBytes()}`. No allocation, no copy — the rows are produced from the live tree (same as `DevicesModule` produces from `devices_`).
- Works on desktop immediately (the Scheduler + self-report are platform-neutral).
- **Shippable alone**: an "expensive-module" view with no config change.

### Step 2 — RTOS task list (platform getter + trace facility)

- New platform seam (domain-neutral, no FreeRTOS type escapes):
  ```cpp
  struct TaskInfo { char name[16]; uint8_t state; int8_t core; uint8_t priority;
                    uint32_t stackFreeBytes; uint32_t cpuPermille; };  // cpuPermille=UINT32_MAX = not measured
  size_t taskSnapshot(TaskInfo* out, size_t maxTasks);
  ```
- **esp32 impl** (`platform_esp32_tasks.cpp`, new file — keeps `uxTaskGetSystemState`/`pcTaskGetName` out of the big platform file): gated `#if defined(CONFIG_FREERTOS_USE_TRACE_FACILITY)`; fills `TaskInfo[]` from `uxTaskGetSystemState`, maps `eTaskState`→our `state` enum, `xCoreID==tskNO_AFFINITY`→`-1`. `cpuPermille` computed from `ulRunTimeCounter/total` only when `MM_TASK_CPU_STATS` (else `UINT32_MAX`). Inert stub (returns 0) when the trace facility is off.
- **desktop stub** returns 0 (no RTOS) — the module then shows only the MoonModule table with a status note for the RTOS half.
- `esp32/sdkconfig.defaults`: add `CONFIG_FREERTOS_USE_TRACE_FACILITY=y` (measured ~1.5 KB heap / ~2 KB flash, **no per-tick cost**).
- A second `ControlType::List` (`tasks`) renders the snapshot: name / state-glyph / core / prio / stack.
- Two `ReadOnly` text controls `core0` / `core1` (multi-core only) via `xTaskGetCurrentTaskHandleForCore` — behind the same platform seam (`platform::currentTaskOnCore(int)` → name), not a raw FreeRTOS call in the module.

### Step 3 — nest modules under their task

- Merge into one `tasks` List whose **row = task**, **row-detail = the modules in that task** (`writeListRowDetail` emits the module rows). Today the association is trivial: `Scheduler::tick()` runs every module on the one render task, so all modules attach to the render task's detail; other tasks have empty detail. The mapping is a simple predicate in the module (`is this the render task? → all scheduled modules : none`), documented as the present single-task reality.
- This is the target nested view; steps 1–2 are the pieces it composes.

### Step 4 — per-task CPU% (opt-in, build-flag)

- `MM_TASK_CPU_STATS` (a compile def, wired in `esp32/main/CMakeLists.txt` / the platform_config) enables both the sdkconfig `GENERATE_RUN_TIME_STATS`+`RUN_TIME_STATS_USING_ESP_TIMER` and the `cpuPermille` fill + the UI column. Off by default (measured ~5 % tick). A profiling build turns it on.

## Files

- **New:** `src/core/TasksModule.h` + `.cpp`; `src/platform/esp32/platform_esp32_tasks.cpp`; `TaskInfo` + `taskSnapshot`/`currentTaskOnCore` decls in `src/platform/platform.h`; desktop stub in `platform_desktop.cpp`.
- **Edit:** `src/main.cpp` (register `TasksModule` + include); `esp32/sdkconfig.defaults` (`USE_TRACE_FACILITY`); `esp32/main/CMakeLists.txt` (the `MM_TASK_CPU_STATS` opt-in def, off by default); `docs/moonmodules/core/system.md` (`### Tasks` section + control table); the module's `///` for the generated page.
- **Tests:** `test/unit/core/unit_TasksModule.cpp` — feed a fake `TaskInfo[]` (via a test seam or the desktop stub returning canned rows) + a small Scheduler with a couple of fake modules; assert the List rows/detail render the expected `{name, us, class, heap}` and task fields. Unit-only — it's a diagnostic, not in the render pipeline, so no scenario.
- **Catalog:** none required — it's a core module addable to any device (like I2cScan); optionally add to a bench deviceModel for convenience.

## Verification

1. `cmake --build build` clean (zero warnings); `ctest` (the new unit test) green; `check_specs.py` green (the `system.md#tasks` docPath + control names match the `.h`).
2. Desktop: the module lists every MoonModule with real `loopTimeUs`/size/heap; the RTOS half shows the empty-stub note.
3. ESP32 (bench): the task list shows all RTOS tasks with core/prio/stack; the render task's detail nests every MoonModule; `core0`/`core1` name the live tasks. KPI tick delta from `USE_TRACE_FACILITY` is within noise (no per-tick cost).
4. Platform boundary check passes (no FreeRTOS symbol outside `src/platform/`).
5. With `MM_TASK_CPU_STATS` built: the CPU% column populates; confirm the ~5 % tick cost is only present in that build, not the default.

## Scope guard (the critical bit)

Do **not** build the relocate UI, a multi-task scheduler, or per-module affinity in this plan — those are Phase 2 and need the 🚧 mechanism first. Phase 1 is the *view*. If step 3's nesting starts to want a general "modules-per-task registry," stop: today it's a one-line predicate (render task owns all), and a registry is speculative until multiple tasks exist.

Save the approved plan here (this file). Mark `(shipped)` when it lands.
