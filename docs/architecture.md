# Architecture

This document is the agreed-up-front **architecture contract**: what projectMM is designed to be. Most of it describes the system as it is today. A design described here is committed (this is the intended behavior, and code is written toward it), not optional or undecided.

Coding conventions live in [coding-standards.md](coding-standards.md); how to build and run lives in [building.md](building.md); what is tested lives in [testing.md](testing.md).

## Contents

- [Architecture](#architecture)
  - [Contents](#contents)
  - [The problem](#the-problem)
  - [Core and light domain](#core-and-light-domain)
- [Core](#core)
  - [MoonModules](#moonmodules)
    - [Lifecycle propagation to children](#lifecycle-propagation-to-children)
  - [Controls](#controls)
  - [Persistence](#persistence)
  - [Parallelism](#parallelism)
  - [Data exchange between modules](#data-exchange-between-modules)
  - [Event triggering between modules](#event-triggering-between-modules)
    - [Live reconfiguration: every change applies without a reboot](#live-reconfiguration-every-change-applies-without-a-reboot)
  - [Robustness](#robustness)
  - [Hot path discipline](#hot-path-discipline)
  - [Platform abstraction](#platform-abstraction)
  - [Firmware vs deviceModel vs board](#firmware-vs-devicemodel-vs-board)
  - [Services](#services)
  - [Multi-device runtime](#multi-device-runtime)
    - [Device name: one identity, every network name derives from it](#device-name-one-identity-every-network-name-derives-from-it)
- [Light domain](#light-domain)
  - [The pipeline](#the-pipeline)
  - [3D from the start](#3d-from-the-start)
  - [Layouts and Layout](#layouts-and-layout)
  - [Effects and Layer](#effects-and-layer)
  - [Effects](#effects)
    - [Dimensionality](#dimensionality)
    - [Robustness rules](#robustness-rules)
  - [MoonLive: the live-script engine](#moonlive-the-live-script-engine)
  - [Modifiers](#modifiers)
  - [Mapping and blending](#mapping-and-blending)
  - [Drivers](#drivers)
  - [Memory strategy](#memory-strategy)
    - [Buffer types](#buffer-types)
    - [Adaptive allocation](#adaptive-allocation)
    - [Degradation cascade](#degradation-cascade)
    - [Invariants](#invariants)
    - [Per-module reporting](#per-module-reporting)
    - [Scaling to available memory](#scaling-to-available-memory)
  - [Multi-device sync](#multi-device-sync)
- [Web UI](#web-ui)

## The problem

Build a modular runtime for resource-constrained embedded devices that the same source compiles for, unmodified, on ESP32, Teensy, desktop, and Raspberry Pi. The runtime must:

- Compose behavior from small, uniform units (modules) that can be created, configured, reordered, and removed at runtime, including from a network API.
- Expose every module's parameters generically so a single web UI renders any module with zero per-module UI code.
- Run a hot loop with predictable timing and zero steady-state heap allocation on devices with as little as ~320 KB of RAM.
- Persist configuration across reboots, exploit multiple CPU cores where present, and keep all platform-specific code behind one boundary.

The first concrete use of this runtime is lighting: drive 10,000+ addressable LEDs and DMX fixtures (RGB(W) pars, moving heads, dimmers) across multiple synchronized devices at high frame rates. The runtime is general enough that other real-time domains (audio synthesis, motor control) could be layered on the same way; lighting is the only domain implemented today.

**Sensors are part of the job, not an accessory.** One of the intended uses is art installations, and an installation people can interact with needs to sense them: a microphone the piece dances to, a button or foot pedal someone presses, a motion sensor that wakes it, a distance sensor that follows a hand. So projectMM commits to supporting the sensors installations actually use, and treats an input peripheral as first-class alongside an output driver. The scope stays deliberate: a lighting controller that senses its audience, not a general home-automation platform, where breadth of device support would be the product. Input peripherals live in the core `Services` container beside Audio and Infrared, and reach the rest of the system by one of two paths depending on what they produce. An EVENT (a press, a learned remote code, a threshold crossing) drives a control through `Scheduler::setControl`, like every other transport. A continuous VALUE is published as a shared frame instead: `AudioService` exposes an `AudioFrame` through `latestFrame()` that audio effects pull per frame, because pushing a stream through a control per sample would serialize it through a settings path. A sensor commonly does both, and the two paths are the [data exchange](#data-exchange-between-modules) rule applied to inputs.

## Core and light domain

The system is two layers, separated as much as practical:

- **Core**: MoonModule base, controls, scheduling, persistence, platform abstraction, system services (HTTP, WiFi, filesystem). Domain-neutral. Knows nothing about lights.
- **Light domain**: light values, layouts, layers, mapping, blending, effects, modifiers, LED drivers, ArtNet/DDP. Built on top of the core.

When mixing is needed (for performance or simplicity), it must be an explicit decision: consciously choosing minimalism over separation, not accidentally blurring the boundary. Use domain-neutral naming in those cases ("producer buffer" not "LED buffer", "output driver" not "LED driver" in core interfaces) to keep the door open for future separation.

**Core primitives, not one-offs.** Core earns growth only by adding a recognizable, reusable primitive many modules lean on (a streaming write, a positional read, a bounded arena, a recursive JSON reader); a core change that only one caller needs is the smell. When a complex system will need a capability, build the cleanest complete version rather than a crippled subset that pushes hacks outward (a JSON reader that can't read arrays is not "minimal").

# Core

The core's job is the runtime: modules, their lifecycle, their parameters, how they're scheduled, how they're persisted, how they reach the platform underneath.

## MoonModules

The core building block is a **[MoonModule](moonmodules/core/moxygen/MoonModule.md)**. Everything is a MoonModule, not just effects, modifiers, layouts, and drivers, but also system infrastructure (HTTP server, WebSocket server, file server, WiFi, mDNS, OTA updates) and [services](#services) (sensors and actuators bridging to hardware/network). The core itself is minimal: MoonModule base, buffer management, a [Scheduler](moonmodules/core/moxygen/Scheduler.md).

This means:

- Every MoonModule shares the same class structure, lifecycle (`setup`, `tick`, `release`), and controls. Learn the pattern once, apply it everywhere.
- System services get controls for free: HTTP port, WiFi SSID, mDNS hostname are all configurable through the same UI as effect parameters.
- Capabilities are modular: no WiFi? don't load the WiFi MoonModule. No `#ifdef`s needed.
- System MoonModules that listen (HTTP, WebSocket) poll in their `tick()`, the standard pattern for embedded servers.
- The scheduler handles init-order dependencies between system MoonModules (e.g. WiFi before HTTP, HTTP before WebSocket).

Modules can be added, replaced, reordered, or removed at runtime. On removal (release), all allocated resources are cleaned up.

### Lifecycle propagation to children

A MoonModule that owns children gets the standard lifecycle methods propagated to them automatically:

- `setup()` and `release()`: chain into children. Teardown reverse-iterates so children clean up before the parent does.
- `tick()`, `tick20ms()`, `tick1s()`: tick each child gated by the same rule the Scheduler applies to top-level modules (`!respectsEnabled() || enabled()`, where modules that opted out of the enabled gate keep ticking, the rest tick only when enabled), with per-child timing accumulated into the child's own `tickTimeUs()`.
- `defineControls()` and `prepare()`: chain into children.

This means a container module gets correct lifecycle handling for its children without writing the iteration itself. Leaf modules (no children) pay one predicted-not-taken branch per call, sub-nanosecond. When a container overrides one of these methods to add its own work, the chain-to-base convention (parent-before vs child-before per callback) lives in [coding-standards.md § Override-and-chain convention](coding-standards.md#override-and-chain-convention).

**ModuleFactory** is a static registry mapping type names (strings) to create functions. The HTTP API uses it to create modules at runtime (`POST /api/modules {"type":"NoiseEffect"}`); the main pipeline in `main.cpp` constructs modules directly. Registration captures `sizeof(T)` for memory reporting:

```cpp
ModuleFactory::registerType<NoiseEffect>("NoiseEffect");
```

ModuleFactory is core infrastructure ([`src/core/ModuleFactory.h`](../src/core/ModuleFactory.h)), not itself a MoonModule.

**Dynamic over fixed-size.** Children, module lists, control sets, anything structural, grow on demand from the heap during `setup()`. Fixed-size arrays impose arbitrary limits, waste memory on instances that don't use the full capacity, and cost memory on instances that need none (e.g. leaf modules with zero children). The hot path only iterates these arrays: same pointer arithmetic as a fixed array, no performance difference.

**Self-reporting.** Every MoonModule reports its own footprint and cost: `classSize()` (the `sizeof` of the class instance, captured at registration), `dynamicBytes()` (heap allocated during `prepare`), and `tickTimeUs()` (average time its `tick` took, accumulated per tick). These surface in `/api/system`, console output, and scenario tests: the same numbers for an effect, a driver, or a system service, because they're a base-class feature, not a light-domain one.

Each MoonModule has two documentation surfaces under `docs/moonmodules/`: an end-user **summary page** — one 4-column table row in its group's page (effects/modifiers/layouts/drivers, or core/light UI/supporting) — and a **generated technical page** built from the header's `///` comments. See [coding-standards § Documentation model](coding-standards.md#documentation-model) for the full model.

## Controls

Every MoonModule exposes **[controls](moonmodules/core/moxygen/Control.md)**: runtime-configurable parameters visible in the web UI. A grid layout exposes width, height, depth. An ArtNet driver exposes destination IP and universe. A fire effect exposes speed, cooling, sparking.

Controls bind to MoonModule member variables. The variable's default is the control's default. The hot path reads the variable directly, no function call. When a control value changes, the system notifies the owning MoonModule for cold-path reactions: recompute a derived table, re-size a buffer, re-bind a socket (the three-tier mechanism is [§ Event triggering between modules](#event-triggering-between-modules)).

Controls are dynamic: when a value changes, the control set can be rebuilt. A select control that picks a mode can show/hide other controls based on the choice.

Prefer `uint8_t` (0–255) for slider controls. Minimises per-control memory, aligns with DMX channel values, keeps the UI range manageable.

Controls are the bridge between the [web UI](moonmodules/core/ui.md) and the running module tree: the UI renders a control from what the MoonModule declares, and a value the user changes there writes straight back into the module's member variable. The exact control types (slider, toggle, color picker, text input, dropdown) are defined in the [UI spec](moonmodules/core/ui.md#control-types). The principle: modules declare what they need, the UI renders it.

## Persistence

Control values and each module's `enabled` flag are persisted to flash so settings survive a reboot. The mechanism lives in [FilesystemModule](moonmodules/core/moxygen/FilesystemModule.md):

- **Storage**: one flat JSON file per top-level module under `/.config/<TypeName>.json`. Children are encoded positionally with `<index>.` key prefixes — a deliberately flat file shape loaded by the cheap first-match key helpers in `core/JsonUtil.h`. A control whose *value* is structured (a List control's array of objects) round-trips that array with the recursive reader in the same header, via the control's own restore hook; the file's top level stays flat, the structure lives inside one control's value.
- **Lifecycle**: `Scheduler::setup()` runs four phases: (1) `defineControls` binds every module's full control set, (2) the FilesystemModule load hook overlays persisted values onto the bound variables, (2b) `rebuildControls` re-evaluates conditional `hidden` flags against the loaded state, (3) each module's own `setup()` runs with persisted values already in member variables, (4) `prepare` sizes buffers. Modules themselves know nothing about persistence; they just bind their variables.
- **Save trigger**: HttpServerModule marks the target module dirty on every successful control mutation. FilesystemModule debounces 2 s in `tick1s()`, walks the tree, writes any subtree containing a dirty descendant via atomic write-and-rename.
- **Conditional controls**: every conditional control is always bound; the module sets a `hidden` flag (`controls_.setHidden(i, …)`) to tell the UI not to render it. The load path can therefore find persisted values regardless of the live conditional state.
- **Code-wired children survive a stale file**: some children aren't created by the user; `main.cpp`'s boot wiring attaches them (`ImprovProvisioningModule` under `NetworkModule`; `NetworkSendDriver`, `PreviewDriver` under their parents). Each such child calls `markWiredByCode()` after `addChild()`, a one-bit flag meaning *"I belong here because the code put me here, not because a saved file or a user asked for me."* The problem it solves: persistence reconciles the live tree to match the saved JSON, so a child that exists in code but is absent from an older saved file (written before that child was added) would be trimmed on load. The flag tells the apply step to keep it. Children added through the HTTP API or recreated from JSON stay unmarked; those follow the file's tree shape exactly, so UI deletes still take effect.

Persistence reaches the Scheduler through a **function-pointer hook** (`setLoadAllHook`) the load phase calls if set: FilesystemModule registers its load routine there at startup, so the Scheduler never names FilesystemModule (no circular dependency, persistence stays optional; a null hook means defaults-only). The choice of a flat POD image over a JSON format, and of load-before-setup, is [ADR-0001](adr/0001-persistence-pod-memcpy-not-json.md).

## Parallelism

On multi-core systems (ESP32 has 2 cores, desktop / RPi have many), the system exploits parallelism by assigning MoonModules to specific cores. Each MoonModule can declare a core affinity. The scheduler respects this when pinning tasks. On single-core or desktop systems, affinity is ignored and everything runs on available threads.

The model is **producers vs consumers**: producers generate data, consumers process and output it. The light domain instantiates it concretely: effects are producers, drivers are consumers.

**The render↔output split.** `Drivers` owns one switch, `multicore` (default on): when it engages, a **core-1 task runs the whole output stage** — every driver's `tick()`, so the LED encode, the ArtNet packet build and the preview frame build all leave the render core — while **core 0** renders the next frame and services HTTP/WiFi/WS. A frame therefore costs `max(render, output)` instead of `render + output`. There is deliberately **no per-driver opt-out**: the container owns the mechanism (the handoff buffer, the task, the frame boundary), so there is one split, not one per driver.

The hand-off is the **single shared output buffer plus a frame boundary**, not a held lock: core 0 waits on an atomic `encodeDone_` before overwriting the buffer, so the cheap composite is the only serialization point and the two heavy stages (render, encode) overlap. It is allocate-and-degrade — the split engages only when a driver exists *and* the handoff buffer allocates, so a memory-tight board never lands in a half-split state; it simply runs every driver inline on core 0 exactly as before, and the split re-engages by itself when the memory is there.

Two contracts make it safe against a live, mutating tree, and both live in **core** so no module has to remember them:

- **`MoonModule::quiesce()`** — core calls it on the parent before every structural mutation (`addChild` / `removeChild` / `replaceChildAt`), because a mutation frees or reallocates memory the worker may be walking. See [§ Controls](#controls) above.
- **`BinaryBroadcaster::tryAcquireSend()` / `releaseSend()`** — the WebSocket sender has **two producers on two cores** once the split engages: core 0 (the transport's own `tick20ms` drain, the 1 Hz state push, connect/disconnect) and core 1 (the offloaded `PreviewDriver`, which arms a frame and streams its coordinate table). A producer brackets its whole message in the lease, so a multi-call stream (`begin`/`push`/`end`) can't have another core's write land between its parts, and a frame arm can't race the drain reading the slot. It is **try-acquire, never blocking** — the hot-path rule forbids a render or encode thread waiting on a peer — so whoever loses the race **skips its slot**, which for the preview is the same back-off its adaptive frame rate already takes when the link is behind. A lost race costs one preview frame; a blocked encode thread would stall the LEDs.

A driver that writes a socket still hands its bytes to lwIP on **core 0** (that is where the stack is pinned): the CPU half offloads, the send itself does not move. That is the intent, not a leak — the measured cost is ~100 µs/frame against ~13,000 µs of output work removed.

Which buffers play the double-buffer role is covered in [§ Memory strategy](#memory-strategy).

## Data exchange between modules

When one module produces data another module reads on the hot path, the pattern is the same throughout the codebase. Two shapes, both core-defined and domain-neutral:

**Shared-struct (pull).** The reader holds a pointer to the producer's data and reads it when it needs it.

- The **producer owns a small POD struct** as a member, overwritten in place each tick. No allocation per frame.
- A **plain-data header** declares the struct. Both producer and consumer include it; neither needs to know the other's class.
- The producer exposes the struct via a `const`-returning getter (or a `setX(const Foo*)` setter on the consumer).
- The **consumer holds a `const Foo*`** received once at wiring time in `main.cpp`, and reads it on the hot path each frame.

No registry, no subscription, no event bus. The consumer reads the latest value when it needs it; if the producer wrote nothing this tick, the consumer sees the previous value (acceptable for the kinds of data this exchanges: small state structs, periodic captures). This pull pattern is lock-free **for a small POD struct overwritten in place**: a reader on another core might catch a half-updated struct, but the result is one slightly-inconsistent read of a few fields that self-corrects next tick, visually harmless for the gyro/sensor data this carries, and cheaper than a lock. That tolerance does **not** extend to a large frame buffer the consumer copies out wholesale (an LED DMA buffer, an ArtNet packet): there a half-written read is a visible glitch, so that hand-off uses the two-core double-buffer swap from [§ Parallelism](#parallelism), not this lock-free pull.

**Push through a domain-neutral sink.** When the producer should hand bytes to a generic core service rather than expose a struct, the core defines a narrow interface and the producer pushes to it. The producer owns the data and its wire format; the core sink (the interface's implementer) knows only "take these bytes and do my generic job"; it has zero knowledge of what the bytes mean or which domain produced them. `BinaryBroadcaster` (`HttpServerModule` implements it: "broadcast these bytes to all WebSocket clients") is the example; the producer side lives in the light domain (see [§ The pipeline](#the-pipeline)).

Both shapes extend to any future producer/consumer pair (a sensor owning a state struct read through a `const Foo*`; a module pushing bytes to a core sink). Neither is pub/sub, and the reasons this project chose pull + a prepare-pass over an event bus are [ADR-0011](adr/0011-data-exchange-pull-and-prepare-pass-not-pubsub.md).

## Event triggering between modules

A control changes, or the module tree is mutated (a child added, deleted, replaced, moved), and other modules may need to react. The framework provides a three-tier split so each change costs only as much as it has to, from cheapest to most expensive:

1. **`onControlChanged(controlName)`**: runs on *every* control change, but only on the module whose own control changed. A cheap, in-place, per-control reaction that touches nothing else: recompute a small derived table, re-bind a socket. Default no-op.
2. **`affectsPrepare(controlName)`**: a gate, default `false`. A module returns `true` only for controls that change the size or shape of its derived state (and thus may ripple to other modules); for controls that just tweak a value in place it stays `false`. When `true`, the framework runs the tree-wide rebuild; when `false`, it doesn't.
3. **`prepare()`**: the module (re)builds its derived state (buffers, tables) for the current control values. Reached via `Scheduler::prepareTree()`, the coordinator-driven sweep that walks every module's `prepare`.

`Scheduler::prepareTree()` fires from two triggers: a tier-2 gate returning true after a control change, **and** any tree mutation (HTTP add/delete/replace/move handlers all call it unconditionally, since a structural change is rare and unambiguously needs a rebuild). Both triggers funnel through the same sweep; each module's `prepare` is idempotent (e.g. an effect only reallocs when its grid count actually changed), so over-rebuilding is wasted work, not a correctness hazard.

**`quiesce()` — the structural path's thread guard.** A module may hand work to another thread (`Drivers` ticks its Driver children on a core-1 task, see [§ Parallelism](#parallelism)), which makes a *structural* mutation dangerous in a way a control change is not: `addChild` reallocates the child array a worker may be walking, and `removeChild` is followed by the caller's `release()` + `deleteTree()`, which frees the very module a worker may be inside `tick()` on. So `MoonModule` declares `virtual void quiesce()` (default no-op) and **core calls it on the parent before every child-array mutation** (`addChild` / `removeChild` / `replaceChildAt`); a module owning a worker overrides it to park that worker. The control path already funnels through `applyState()`/`prepareTree()`, where the owner quiesces itself — this is the same rule extended to the sibling (structural) path, and it lives in core so no HTTP handler has to remember it (CLAUDE.md § *when core already owns a mechanism for one path, extend it to the sibling path*). Deleting a driver from the UI mid-encode is therefore safe by construction, not by handler discipline.

This is the recognised layout/prepare-pass pattern (JUCE `prepareToPlay`, UIKit `layoutSubviews`, gated by per-object metadata like WPF's `AffectsMeasure`, here `affectsPrepare`); the pull-and-prepare-pass-not-pub/sub decision is [ADR-0011](adr/0011-data-exchange-pull-and-prepare-pass-not-pubsub.md). The light domain consumes it for the mapping rebuild ([§ Mapping and blending](#mapping-and-blending)); the mechanism itself is core.

### Live reconfiguration: every change applies without a reboot

A property that falls out of the three tiers, and sets projectMM apart from most LED-controller firmware (where changing a pin map, strand length, or protocol means a **reboot**): **every MoonModule reconfigures live the instant a control changes — no configuration change needs a restart.** A pin, leds-per-pin, protocol, or mic-rate edit flows control-write → `onControlChanged` (tier 1) and, when it changes shape, → `prepare()` (tier 3), which rebuilds exactly the derived state that changed (an LED driver re-targets its RMT/DMA onto the new GPIOs, an audio module re-inits I²S, an effect re-sizes, the Layer rebuilds its LUT); the render loop reads it next tick. This holds for every module type because the rebuild chain is core, and it composes with the [robustness rule](#robustness): any change, any order, keeps the device running. Only a *firmware* OTA flash needs a power cycle, the same physical boundary the robustness rule draws.

If a module needs to actively notify a specific other module of an event (rather than publish data for polling, or change its own controls), the pattern is a direct method call from the producer to a known consumer: `ImprovProvisioningModule::tick1s` calls `networkModule_->setWifiCredentials(...)` when credentials arrive over UART. No event bus; the producer holds a pointer to the consumer set at wiring time (`main.cpp`). Pub/sub becomes the right pattern only when there are multiple unknown subscribers per event; projectMM has none today.

## Robustness

A running device must tolerate **any sequence of UI actions or API calls** (add, delete, replace, move, or reconfigure any module in any order, at any grid size) and keep running. Degraded or idle is an acceptable outcome; a crash, a hang, or a boot loop is not. This is a defining strongpoint: the device is something an end user can poke at freely without bricking it.

The contract is bounded to **what the software accepts as input**. Power loss, a malformed OTA image, a brown-out, or electrical faults are out of scope; the firmware can't intercept those. Everything that arrives through the HTTP API, the WebSocket, or the UI is in scope.

Why this needs stating as its own guarantee: the mutation-driven rebuild above ([§ Event triggering](#event-triggering-between-modules)) means a single API call can free and rebuild a large slice of the module tree mid-render. The hazard is **stale references**: a module holding a pointer to something that was just torn down. The two patterns that keep it safe:

- **Resolve links at `prepare`, don't cache them across mutations.** A module that depends on another (a `Drivers` reading the active `Layer`, a `Layer` reading its `Layouts`) re-resolves that link from the tree at every rebuild rather than pinning a pointer once at wiring time. When the dependency is gone, the link resolves to null, not to freed memory.
- **Tolerate null at the point of use.** Every consumer of a resolved link null-checks it and falls back to an idle state (no buffer, zero lights, nothing sent) rather than dereferencing. A driver with no Layer sends nothing; a Layer with no Layouts reports zero lights. Idle, not crashed.

The enforcement is the test framework, not discipline alone (see the [Robustness principle](../CLAUDE.md#principles)). When a sequence is found that crashes or wedges the device, the fix is **incomplete until a test reproduces that sequence**, so the same break can't return. Worked example: deleting the last Layer once left `Drivers` holding a dangling pointer to the freed Layer; `PreviewDriver` then read it and panicked (`LoadProhibited`), and because the tree persists, the device boot-looped. The fix made `Drivers` clear its drivers' Layer pointers to null when no Layer is active, and a regression test (`unit_PreviewDriver`, "tolerates the active Layer being deleted") drives a Layer delete + rebuild and asserts the driver ends up null, not dangling. The scenario layer adds the same coverage end-to-end: `clear_children` lets a scenario clear a container and rebuild its own pipeline from any starting tree, so the delete/rebuild path is exercised on real hardware, not just in unit tests.

## Hot path discipline

The render loop (`Scheduler::tick` and everything it calls: every effect, modifier, driver, layout) is the hot path. It runs roughly 50–10000 times per second depending on light count and CPU performance. Code there obeys three rules:

- **No heap allocations.** `new`, `malloc`, `push_back`, `std::string` constructors, `make_unique`, `make_shared`: none of them on the hot path. Heap fragmentation on a long-running ESP32 kills throughput in minutes. Allocate everything during `setup()` / `prepare()`; the loop only reads and writes pre-sized buffers.
- **No blocking.** No `delay`, no `sleep`, no `mutex.lock()`. If a mutex is unavoidable, use `try_lock` and skip the work this tick. Blocking the render task means a visible glitch on the LEDs.
- **Integer math preferred over `float` in per-light work.** ESP32's FPU is single-precision and not as cheap as integer ALU; per-light float compounds fast. Use fixed-point or scaled integer math where the visual difference doesn't justify the cost.

**Memory layout** is the corollary: allocate buffers as single contiguous blocks outside the hot path. Never allocate many small scattered objects in a loop; fragmentation catches up even off-path. On ESP32 with PSRAM, use `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)` for large buffers; the `platform::alloc` wrapper does this automatically.

**Network input** follows the same discipline: process synchronously at a defined point in the frame loop. Async input with staging buffers is allowed when memory is plentiful (desktop, PSRAM-rich ESP32), but the default is synchronous to keep the loop's worst case predictable.

**Data over objects.** The hot path is designed around plain contiguous data: flat buffers one stage writes and the next stage reads, no per-element objects, no virtual calls per light. The module tree is the one deliberate class hierarchy (uniform polymorphism is what lets the UI render any module generically, see [§ Web UI](#web-ui)); off the hot path, a proven adapter interface is fine, e.g. `ListSource`: the textbook data-source shape (UITableView's data source, Qt's `QAbstractItemModel`).

**The sub-hot path is a hot path too.** `tick20ms()` and `tick1s()` run inline on the render thread between two frames, so any code on a timer is on the hot path the moment it fires; a heavy periodic step shows as a stutter at exactly that tick's cadence (a 1 Hz hitch for `tick1s`). Periodic work is therefore *cheap per firing* (bounded, no O(tree) serialize of unchanging data), *amortized* across ticks (drain a chunk per tick, like the preview/state resumable sender), or *gated on a real change signal* so the common case is near-zero. Never re-serialize data that doesn't change: the canonical example is the WS control `optionSets`, emitted once per list and referenced by `optionsRef` rather than re-inlined every `tick1s` (which would be a ~20 KB/s serialize spike on the render thread). The KPI tick timing is the guard; a spike at a tick's cadence is the tell.

## Platform abstraction

Only abstract what you actually need:

- **Time**: `millis()`, `micros()`. Monotonic, microsecond resolution. (`esp_timer` / `std::chrono`)
- **Memory**: `alloc(size)`, `free(ptr)`. Prefers PSRAM on ESP32, falls back to regular heap. `freeHeap()`, `maxAllocBlock()` for diagnostics. (`heap_caps_malloc` / `std::malloc`)
- **Executable memory**: `allocExec(size)` / `freeExec(ptr, size)` allocate memory the CPU can *fetch and execute* from, and `writeExec(dst, src, len)` copies emitted machine code into it safely. Used by the MoonLive live-script engine (below) to place the native code it compiles. All the W^X / instruction-cache quirks live behind these three functions: ESP32 IRAM via `MALLOC_CAP_EXEC` with 32-bit-aligned stores plus a cache sync so the core fetches fresh code; an `mmap` `PROT_EXEC` page on POSIX desktops (macOS-arm64 `MAP_JIT` + a write-protect toggle), a `VirtualAlloc` `PAGE_EXECUTE_READWRITE` page plus `FlushInstructionCache` on Windows. (`heap_caps_malloc(MALLOC_CAP_EXEC)` / `mmap` / `VirtualAlloc`)
- **Networking**: `UdpSocket` for ArtNet send. `TcpConnection` / `TcpServer` for HTTP + WebSocket; `TcpConnection::writeSome` is a non-blocking partial write (returns bytes written, 0 = would-block) so a backpressured browser can't stall the render loop. (lwIP sockets / BSD sockets)
- **Scheduling**: `yield()` (cooperative yield to OS/RTOS), `delayMs(ms)` (blocking sleep, off-path only), `delayUs(us)` (microsecond busy-wait, only for sub-millisecond hardware timing a driver owns — e.g. the WS2812 ≥300 µs inter-frame latch in `RmtLedDriver`; never for general pacing, which uses the non-blocking `millis()` gate), `reboot()`. (`vTaskDelay` / `esp_rom_delay_us` / `esp_restart` on ESP32; `std::this_thread::sleep_for` / `std::exit` on desktop)
- **Platform config**: `platform_config.h` per platform: compile-time constants like `hasPsram` and `hasWiFi`. Each platform provides its own version; `types.h` includes it without `#ifdef`. Core code branches on these via `if constexpr` (e.g. NetworkModule drops its WiFi cascade when `hasWiFi` is false), so the dead branch is removed from the binary with no `#ifdef` outside `src/platform/`.

Abstractions are added when a concrete implementation needs them, not pre-designed.

**Platform boundary (hard rule).** All `#ifdef`, `#if defined`, platform-specific `#include`s, and hardware API calls live exclusively in `src/platform/`. Everything outside `src/platform/` compiles on every target without modification. Compile-time platform branching uses `if constexpr` on `platform_config.h` flags, never a preprocessor `#ifdef`. The boundary is enforced by [`moondeck/check/check_platform_boundary.py`](../moondeck/check/check_platform_boundary.py), a commit gate (see [CLAUDE.md § The Process](../CLAUDE.md#the-process)).

**The desktop build runs everything (hard rule).** Every module, effect and driver in the repo
links and runs on the host — the platform layer simply has no silicon behind the call. Where a
peripheral is absent the host *emulates* it rather than declaring itself incapable: the parallel
WS2812 buses are backed by heap buffers, `lcdLanes` / `parlioLanes` / `rmtTxChannels` report a
real chip's counts, and `hasLcdCam` is true. Code excluded from the host binary is code that
cannot be unit-tested, cannot be seen by any AST-based check, and only ever runs where it is
hardest to debug — which is exactly what the LED drivers were until they were linked here.

A capability flag therefore answers *"can this build exercise the path?"*, not *"is this real
hardware?"*. Where a flag must mean the latter (`hasLcdCam` gating the pin expander), that is a
deliberate, commented exception. Timing, wire protocol and pin state are NOT emulated: they need
silicon, and faking them would let a self-test report on hardware it never touched.

## Firmware vs deviceModel vs board

Three distinct things, kept distinct in the vocabulary:

- **firmware** — the compiled binary (chip target + which radios/peripherals are built in).
- **deviceModel** — the whole assembled product, identified by its catalog name (`Olimex ESP32-Gateway Rev G`). This is *which hardware this is*. It is distinct from **`deviceName`**, *which individual unit this is* (per-unit identity the user sets — see [§ Device name](#device-name-one-identity-every-network-name-derives-from-it)); a **device** (the umbrella term) has a `deviceName` and a `deviceModel`.
- **board** — the bare PCB *only*. The word survives in its literal sense: **on-board** LED, **on-board** peripherals, board-soldered pins — things physically *on the PCB*. (A deviceModel is a board plus whatever is wired onto it.)

**Firmware** is the compiled binary: chip target plus which radios/peripherals/sdkconfig fragments are included. Today's variants: `esp32` (classic, WiFi **and** RMII Ethernet in one binary — Ethernet comes up only when a PHY is present, pins/PHY per deviceModel), `esp32-eth` (classic, Ethernet only, WiFi excluded), `esp32-16mb` (classic with 16 MB flash, WiFi + Ethernet), `esp32s3-n16r8` / `esp32s3-n8r8` (S3 with WiFi + W5500 SPI Ethernet), `esp32p4rev1-eth` (Waveshare ESP32-P4-NANO, Ethernet only), `esp32p4rev1-eth-wifi` (the same P4 hardware with WiFi via its on-board ESP32-C6 over esp_hosted), `esp32p4rev3-eth` / `esp32p4rev3-eth-wifi` (the same two images built for P4 **v3.x** silicon, which is not binary-compatible with rev <3.0 — untested, no v3 board on the bench). Each chip's firmware carries the Ethernet *driver(s)* it can host (RMII EMAC for classic/P4, W5500 SPI for S3); which PHY/pins a deviceModel uses is runtime config. Selected by `build_esp32.py --firmware <key>`, reported by `SystemModule.firmware`, used as the contract target key in scenarios.

**deviceModel** is the physical hardware: chip + PCB + on-board peripherals (PHY, USB-serial, PSRAM, antenna), identified by its product name. Examples: `Olimex ESP32-Gateway Rev G`, `LOLIN D32`, `Generic ESP32 Dev`. A unit cannot identify its own deviceModel (no readable PCB ID on classic ESP32), so MoonDeck deduces it from the firmware where unambiguous (`esp32-eth*` ⇒ Olimex) and otherwise lets the user pick. It is stored on the unit as SystemModule's `deviceModel` Text control (display-only in the UI; HTTP `/api/control` writes still apply). MoonDeck mirrors the picked / deduced value to the unit via `POST /api/control` after each discover and after every dropdown change. The catalog of valid deviceModels lives at [mooninstaller/deviceModels.json](../mooninstaller/deviceModels.json), shared between MoonDeck and the web installer: MoonDeck reads it for its dropdown and HTTP push (plain REST on the LAN); the web installer reads it for its picker and pushes the whole entry — deviceModel plus every module/control — over serial during provisioning as REST ops (**"Improv = REST over serial"**, the `APPLY_OP` vendor RPC; see [ImprovProvisioningModule.md](moonmodules/core/moxygen/ImprovProvisioningModule.md)). Pushing over serial sidesteps the mixed-content block that stops an HTTPS installer page from POSTing to an `http://` device; an already-running device is re-configured via MoonDeck on the LAN.

A deviceModel can run multiple firmwares (the Olimex Gateway runs both `esp32-eth` and the default `esp32`); a firmware can run on multiple deviceModels (`esp32` runs on any classic ESP32 dev kit). The `esp32s3-n16r8` firmware is S3-only and does not run on the Olimex Gateway or other classic-ESP32 hardware. The codebase reserves "deviceModel" exclusively for the physical product and "firmware" exclusively for the compiled binary.

### MoonBase: the second boot image

Dual-OTA spends half the app area on a second copy of the firmware that is idle except during an
update. **MoonBase** replaces it: a small, rarely-changing image in the partition table's
`factory` slot that owns the device while the application is being replaced, since a board
cannot rewrite the partition it is executing from. One app slot then suffices, and the flash the
second slot held goes elsewhere.

A 4 MB board has no choice, having room for one application and not two; its app slot grows by a
third in exchange. On a **16 MB** board the choice is deliberate rather than forced, and the
freed 4 MB goes to the filesystem (11 MB rather than 7). Which boards use MoonBase is a
per-variant decision recorded in `moondeck/build/build_esp32.py`, not a property of flash size:
today the 4 MB classic, the S3-Zero and `esp32-16mb` do, and it may become the default
everywhere.

The update cycle: the app stages the install URL in NVS (or nothing, for a browser upload),
points the bootloader at MoonBase and reboots; MoonBase joins the network with the app's stored
credentials (AP fallback at 4.3.2.1), installs into the single app slot, from the staged URL
unattended, or from an upload, and reboots back. The UI covers the whole cycle with one
"updating firmware" overlay, telling the two images apart via `GET /moonbase` (MoonBase answers
with its live status; the app 404s it). Pointing the bootloader at a factory partition *erases*
otadata, so a power cut anywhere mid-install boots MoonBase and the user retries over the
network, a stronger power-fail story than dual-OTA's. A failed install deliberately stays in
MoonBase, visibly, rather than silently reverting to the old app; the way back is its explicit
"Boot the app" action, which only boots an image that validates.

**Updating MoonBase itself** runs the same cycle backwards: the app writes the factory slot while
running from `ota_0`, exactly as MoonBase writes the app slot while running from factory. Neither
image can rewrite the partition it executes from, so each installs the other and the app is the
only thing that can repair a broken recovery image. Without it a bad MoonBase means a cable, which
is the failure MoonBase exists to prevent.

Two things make that safe enough to offer. `esp_ota_*` refuses a factory partition, so this is a
raw `esp_partition_erase_range` + `esp_partition_write`, which also forfeits the validation
`esp_ota_end` performs: `esp_image_verify` replaces it after the write. And because a 4 MB board
has nowhere to stage 743 KB before erasing, the image streams straight in, so everything that can
reject it is decided from its FIRST CHUNK, before a byte is erased: the image magic, the chip id
(one MoonBase per chip, one paste apart, and a checksum does not catch a swap), and the descriptor
naming `projectMM-moonbase` rather than the app. Those rules live in `src/core/FirmwareImage.h` so
a host test can drive them. What remains is a window, during the write, in which the device holds
no recovery image; the app keeps running throughout, so the answer to a failure is to retry.

Each image reports its version from the app descriptor IDF puts in every binary, `PROJECT_VER`
being set to the same computed version for both, so the app can read the factory partition's
version without booting it and say when the two were built apart. A device that cannot name its
own recovery image cannot be diagnosed: two boards that looked identical, one of which could not
install firmware, took a bisect of the git log to tell apart.

MoonBase is a standalone ESP-IDF project (`moonbase/`, ~750 KB against an 896 KB slot) sharing
no sources with the app, the deliberate trade for an image that must stay small and, once
working, hardly change. `moondeck/build/build_esp32.py` builds it alongside every variant that opts in
and owns the flash-layout helpers every consumer uses (serial flash, mooninstaller manifests,
release preview, the QEMU image): IDF's own `flasher_args.json` knows nothing of the two-image
scheme and stages the app at the factory offset, so each of those paths applies the same
correction from one place. Prior art: Tasmota's safeboot scheme and
[MycilaSafeBoot](https://github.com/mathieucarbou/MycilaSafeBoot) proved the single-slot +
recovery-image pattern; MoonBase is our from-scratch, minimal take on it.

### Config provenance: MCU → deviceModel

Firmware-vs-deviceModel is a **two-level** model for **where a pin or setting default legitimately comes from**. The installer and MoonDeck use it so a user picks their hardware instead of hand-typing every GPIO. A default belongs at the level that actually *fixes* it:

- **MCU → firmware.** The chip (classic / S3 / P4) and the compiled binary. Fixes silicon- and build-wired facts: native-radio presence, PSRAM, and **which Ethernet *driver* is compiled in** — RMII EMAC (classic/P4) vs W5500 SPI (S3), i.e. `hasEthernet` and the driver kind. These are the compile-time `hasI2sMic` / `hasWiFi` / `hasEthernet` constants in `platform_config.h`; the firmware variant *is* the MCU choice. The firmware also ships a **per-chip default eth pin *seed*** (`platform::ethConfigDefault`) — a fallback so an un-configured unit at least attempts a sensible map — but that is only a seed, *not* the truth for any specific product (see below).
- **deviceModel → the assembled product.** Everything physical about a specific product, *overriding the firmware seed where the product differs*. The **actual** Ethernet PHY pin map + PHY type + MDC/MDIO/clock for this product (the catalog entry pushes them via `setEthConfig`, replacing `ethConfigDefault` — e.g. the Olimex Gateway's `ethType:1, ethRstGpio:5, ethClockGpio:17` are Olimex-specific, not the generic-classic seed), plus C6 SDIO pins, button pins, the on-board status LED, **and** whatever else is wired on the product (a mic, LED strands, a loopback jumper). One catalog entry per deviceModel captures all of it.

So the Ethernet pins live at **both** levels, and that's not a contradiction: the firmware *seeds* a per-chip default, the deviceModel *fixes* the real map. The driver (which Ethernet stack) is firmware-only; the pin map is firmware-seeded but deviceModel-authoritative.

**The deviceModel is one level — there is no separate per-unit provenance level.** Whether a control is PCB-fixed or user-wired is not a taxonomy the code tracks; it falls out of what the catalog entry lists. A bare dev kit lists few controls (the user wires the rest, so those stay unset); a finished product lists more (its wiring is fixed). Same kind of entry, different completeness, no `kind:` flag.

**The governing rule — "default only where the hardware actually fixes it"** — is the [Defaults rule](coding-standards.md#defaults) applied to pin provenance: an entry defaults a control by *including* it and leaves a user-wired control unset by *omitting* it, so the data carries the rule with no level-tagging. It covers **settings, not just pins**: `txPowerSetting` is set per-entry because whether a rig sustains full-power WiFi TX is a brownout property of the assembly and its power supply, not the chip (the catalog pins `Network.txPowerSetting: 8` for the ESP32-S3 N16R8 Dev, which browns out at full power on typical USB). The catalog is [`mooninstaller/deviceModels.json`](../mooninstaller/deviceModels.json) (schema in the [installer README](../mooninstaller/README.md)).

## Services

A **service** is a MoonModule (role `ModuleRole::Service`) that bridges to the outside world (hardware or network) *independently of the light pipeline*. Examples: a gyro/IMU over I²C, a microphone over I²S, a relay or GPIO toggled out, a status push to Home Assistant. Services are **domain-neutral and live in core**; the platform transport they use (I²C, UART, GPIO) is itself a domain-neutral platform primitive.

> *"Service" here means a user-added capability bridge, not the ESP32's own on-chip peripherals (LCD_CAM, SPI, PARLIO, RMT). Those on-chip blocks are how **drivers** clock data out to LEDs, reached through the [platform layer](#platform-abstraction).*

The defining line is the **data relationship, not the connector**: *does the module consume the light output buffer?* If yes it's a **driver** (ArtNet, DMX, SPI-LED all consume the buffer, differing only in transport; a DMX sender uses a UART/RS-485 transport but is a driver because it sends the rendered buffer). If no, it's a **service**.

Services are **user-add/deletable children of the `Services` container** — the core-domain twin of the light pipeline's `Effects`/`Drivers`: a top-level container holding user-added children of one role. The firmware is identical whether or not the hardware is wired, so the user adds the module when they solder a gyro on and removes it later, reusing the generic child add/replace/delete + persistence machinery (`Services` declares `acceptsChildRoles("service")`). Fixed device infrastructure (identity, network, the inspection tools Tasks/I2cScan) lives under **System** instead, wired by code, not user-added — that is the System/Services split. Direction is per-module, not a role: a service may read (gyro), write (relay), or both, so one `Service` role spans the category. Each is a header-only or `.h`+`.cpp` core module under `src/core/`, reaches hardware only through a domain-neutral platform primitive (`platform::i2c*`, `platform::audioMic*`, …), and gets a spec in `docs/moonmodules/core/services.md` (enforced by `check_specs.py`). Most poll in `tick20ms`/`tick1s`; the exception is a service whose data an effect consumes *every frame*: [AudioService](moonmodules/core/moxygen/AudioService.md) reads + analyses its I²S microphone in `tick()` because the audio effects react per render tick, and its per-tick cost (one FFT) is part of the render budget. Automatic bus-probe detection is out of scope; the manual path is the foundation.

**An effect reads a service's data** via the shared-struct pull pattern from [§ Data exchange](#data-exchange-between-modules), no new mechanism: the service owns a small POD struct overwritten in place each poll/tick, and the consuming effect holds a `const` pointer to it. The first concrete case is audio: AudioService produces an `AudioFrame` (level + 16-band spectrum + peak) that [AudioSpectrumEffect](moonmodules/light/effects.md) and the other audio effects consume. It reaches the frame through a static `AudioService::latestFrame()` rather than a boot-time setter, a small variation on the pattern, because an audio effect can be added through the UI *after* boot and must still find the one live mic (a setter only wired the boot instance). The active mic registers itself in `setup()` and clears the pointer in `release()`, so add/remove in any order returns either the live frame or a static silent one, never null. A service that only *displays* its readings (the gyro today) skips the consumer side entirely.

## Multi-device runtime

Two domain-neutral services let several controllers act as one installation. They're core because nothing about them is light-specific; any domain spanning multiple devices uses the same two.

- **Discovery**: devices find each other via mDNS. `NetworkModule` advertises each device today; this is live.
- **Clock sync**: a shared monotonic clock is the foundation any cross-device coordination builds on. The design is filed in [backlog-core](backlog/backlog-core.md).

What the synced clock is *for* is a domain question; the light domain's use of it (synced animation across a wall) is in [§ Multi-device sync](#multi-device-sync).

### Device name: one identity, every network name derives from it

A device has **one** network name, `deviceName`, and every name the device presents on the network is that exact string: the mDNS hostname (`<deviceName>.local`), the SoftAP SSID (the captive-portal network shown when unprovisioned), and the DHCP hostname (what the router's client list shows). They are not three settings that happen to match — there is a single source and the others *read* it, so a device shows one identity everywhere and the three can never drift apart.

- **Sole owner: `SystemModule`.** `deviceName` is a control on `SystemModule` (default `MM-XXXX` from the MAC). It is the only place the name is stored or edited. Every consumer reads `SystemModule::deviceName()`; no other module holds a name of its own. `NetworkModule` reads it for the mDNS / AP / DHCP names; `main.cpp` reads it for the `MM_DEVICE=<deviceName>.local` boot-serial token the [web installer](../mooninstaller/README.md) uses to offer a clickable `.local` link. So to know what name a device advertises, you read one accessor — you never inspect NetworkModule or the platform to discover it.
- **Always a valid hostname.** Because all three uses are DNS/SSID names, `deviceName` must satisfy the RFC-1123 label rules (`[A-Za-z0-9-]`, no spaces, no leading/trailing hyphen). `SystemModule` enforces this at the source: it runs `mm::sanitizeHostname()` (in `core/Control.h`) on the value in `setup()` and every `tick1s()`, coercing whatever the user typed or persistence restored (`"My Living Room!"` → `"My-Living-Room"`) and falling back to the MAC-derived `MM-XXXX` if the result is empty. Sanitising *at the owner* means every consumer is correct for free — no per-consumer validation, no chance a raw name reaches mDNS. (`unit_sanitizeHostname` pins the rule.)
- **Follows a live rename.** Renaming the device re-advertises immediately, no reboot — the [live-reconfiguration](#live-reconfiguration-every-change-applies-without-a-reboot) rule applied to identity. `NetworkModule::syncMdns()` (called each `tick1s()`) compares the current name to the last-registered one and re-registers mDNS when it changed, so `<new-name>.local` resolves within a tick.

**A machine-facing identity that an external system binds to is never the editable name.** An MQTT topic prefix, a Home Assistant discovery `unique_id`, an API key path: anything a foreign system keys off must derive from an immutable hardware id (MAC / chip-id, e.g. `projectMM/<last6-of-MAC>`), because a live `deviceName` rename would silently repoint every topic and orphan the peer's config. The human-readable name rides a *separate*, published-but-non-identifying field (WLED, Tasmota, ESPHome, and HA discovery all anchor identity this way). `deviceName` above is the network-presentation identity (mDNS / AP / DHCP, where the name *is* the address); an external-integration identity is the opposite case and stays decoupled from it.

# Light domain

The light domain is everything specific to driving lights. **Light** here means any controllable light source: an addressable LED pixel (WS2812, APA102), a DMX fixture (RGB par, moving head, dimmer), or any other output that takes color/intensity data. The term is used instead of "pixel" because the system controls both LEDs and conventional lighting fixtures.

## The pipeline

Modules in the light pipeline can be added, replaced, or removed dynamically at runtime.

```text
              Layouts (shared by every Layer in Effects)
                ├── GridLayout  ──→ coordinate iterator
                └── WheelLayout ──→ coordinate iterator
                        │
                    Effects
                ┌───────┼───────┐
                ▼       ▼       ▼
            Layer A  Layer B  Layer C
          Effect(s) Effect(s) Effect(s)
        Modifier(s) Modifier(s) Modifier(s)
        Buffer(own) Buffer(own) Buffer(own)
            LUT(own) LUT(own) LUT(own)
                │       │       │
                └── Blend+Map ──┘
                        │
                    Drivers          (owns Correction: brightness + lightPreset)
                ├── WS2812Driver  ─ apply Correction ─→ DMA buffer
                ├── ArtNetDriver  ─ apply Correction ─→ UDP packets
                └── PreviewDriver (raw buffer, no Correction) ─→ WebSocket
```

**Data flow.** The pipeline instantiates both core data-exchange shapes (see [§ Data exchange between modules](#data-exchange-between-modules)):

- *Shared-struct (pull):* `Drivers` hands every child driver a `Buffer*` (source) plus a `Correction*` (shared brightness/reorder/white), and `Layer` exposes its pixel buffer to `Drivers` directly on the identity-mapping fast path: each consumer holds a `const`-pointer and reads it per frame. The pointers are **(re)bound on every rebuild**, not just at boot: `Drivers::prepare()` re-resolves the active `Layer` (`Effects::activeLayer()`) and calls `passBufferToDrivers()`, which re-runs `setSourceBuffer()`/`setLayer()` on each child (clearing them to `nullptr` when there is no active Layer). So a held pointer is valid only until the next rebuild — which is exactly why the consumers re-read it each frame and tolerate a null (the [robustness rule](#robustness)): a Layer add/delete/replace re-binds or clears it live, no dangling reference.
- *Push to a core sink:* `PreviewDriver` owns the preview wire format (a one-time coordinate table + per-frame RGB point list) and pushes the bytes to a `BinaryBroadcaster` (the core HTTP server). The server broadcasts them over WebSocket without knowing they're a preview: the format and the light types stay entirely in the driver. See [PreviewDriver](moonmodules/light/moxygen/PreviewDriver.md).

**Two WebSocket channels, by traffic class.** `/ws` carries the control plane (JSON state and patches); `/wsp` carries lossy binary streams (the preview). They are separate TCP connections on purpose: preview frames are large and droppable while state messages are small and latency-sensitive, and sharing one connection makes the small ones queue behind the big ones, head-of-line blocking, which surfaced as a flickering connection indicator and an unresponsive UI on large layouts. Separate connections is the standard remedy for that mixed-criticality case.

`BinaryBroadcaster` stays domain-neutral through this: the core still only takes bytes and broadcasts them, with no knowledge that they are a preview. One query serves the producer, `subscriberCount()` for the status line; the work gate is the pull model itself (no standing request, no work). Inbound client frames are unmasked by the transport (framing is its job) and the payload bytes are handed opaquely to the registered `ClientMessageSink`; only the producer knows a `[0x51][stride][fps]` standing request or a `[0x52][stride]` table request from any other bytes. The two channels have separate caps (`MAX_WS_CLIENTS` 8, `MAX_PREVIEW_CLIENTS` 4) because both draw on one `CONFIG_LWIP_MAX_SOCKETS` budget of 16, shared with HTTP, mDNS, Art-Net, MQTT and OTA.

**Graceful degradation under transport backpressure.** The preview is a PULL channel: a client posts a standing `[0x51][stride][fps]` request plus one-shot `[0x52]` table requests, and the device serves the most conservative standing request, building nothing at all when none stands. Every `/wsp` message rides ONE resumable per-client-cursor drain: each socket takes bytes at its own TCP pace on the transport tick, a frame offered while the slot still drains is dropped at the source, and a client is closed only on a real error or FIN, never for slowness. Congestion therefore costs preview frames, never LED time and never a disconnect. Each frame header reports the drops since the last delivered one, and the browser's controller (one pure function in `preview-adapt.js`, unit-tested) reads only that signal: persistent drops coarsen the lattice, drop-free windows refine it a rung at a time, and a refine that brings drops back is taken back with exponentially growing patience (the abandon-fast retry-slowly rule of adaptive-bitrate players). Geometry is cached client-side per (epoch, stride), so a stride change to a known rung costs no table traffic. There is no display cap: the bounds are device memory and the index type, and everything else degrades where it actually binds. The channel machinery is core and domain-neutral (opaque request bytes forwarded to a registered producer sink); `PreviewDriver` is one producer, so other bulky streams can ride the same transport.

**Naming convention.** Capital `Layouts`, `Effects`, `Drivers` are class names (always capitalised when referring to the class). Lowercase "layouts", "layers", "drivers" is the English plural, used freely when context makes it clear. Singular "layout", "layer", "driver" is an individual instance.

## 3D from the start

The system is natively 3D. Coordinates, effects, layouts, and mappings all operate in 3D space (x, y, z). 2D and 1D are simply the case where one or two dimensions have size 1. There is no separate 2D mode; everything is 3D, and lower dimensions fall out naturally.

Two numeric typedefs keep memory tight in LUT tables:

- **`nrOfLightsType`**: total light count, light indices, LUT destinations, `width * height * depth` products. `uint16_t` on devices without PSRAM (max 65 K), `uint32_t` with PSRAM (supports large hub75 panels). Selected at compile time via `platform_config.h`.
- **`lengthType`**: coordinates and dimensions. Always `int16_t` (max 32767 per axis, supports negatives for out-of-bounds effects).

For 12 K LEDs with a 1:1 LUT, the smaller `nrOfLightsType` on no-PSRAM devices saves 24 KB. All code uses the typedefs consistently to avoid casting.

## Layouts and Layout

**Layouts** (a MoonModule) is the top-level container for one or more layouts, defining the physical topology of the installation. It is shared by every layer: there is one Layouts describing the physical setup, and every layer renders into it. When a layout changes, every layer rebuilds its LUT.

A **layout** (a `LayoutBase` MoonModule, child of Layouts) defines the physical positions of lights in 3D space. It is a **coordinate iterator**: it yields `(physicalIndex, x, y, z)` for each light it defines. A layout does not own or build any mapping LUT.

Layouts cover both addressable LEDs and DMX fixtures. An LED-strip layout yields one coordinate per LED; a DMX-fixture layout yields one coordinate per fixture (a moving head is one point in 3D space).

Positions are computed algorithmically, not stored. Grid is the most commonly used layout, but any geometry works: spheres, rings, cones, spirals, arbitrary point clouds. Grid is full-density (every position maps to a light); a wheel is sparse (only spoke positions are mapped, gaps are unmapped).

Multiple layouts can live in one Layouts container. Each layout describes one light type: the model is one light type per layout (LED strips, or par lights), not mixed in a single Layouts.

## Effects and Layer

**Effects** (a MoonModule) is the top-level container for one or more layers. Each layer renders independently into its own buffer; the Drivers container composes those buffers downstream.

**Multi-layer composition.** The container composes more than one Layer's buffer into the shared output: each enabled Layer renders into its own buffer, and the Drivers container's blend+map step composites them in container order (bottom→top) into the physical buffer (which is why that buffer is a *blend* buffer in [§ Memory strategy](#memory-strategy)). Each Layer carries a `blendMode` (alpha-over or additive) and an `opacity` — inert parameters the Layer never acts on; Drivers reads them and the container child order, and blends bottom→top. The bottom layer clears + overwrites the output; each layer above blends onto the accumulated frame per its mode and opacity. With a single enabled Layer this is the degenerate case: a thin pass-through that hands the driver the Layer's buffer directly (no composite), byte-for-byte the single-layer pipeline. The blend math is integer-only per the hot-path rule (8-bit alpha-over `(src·α + dst·(255−α))/255`, additive sum-with-clamp); cost scales with the enabled-layer count.

A **Layer** (a MoonModule, child of Effects) owns:

- A **buffer**: the light data effects write into (logical space).
- A **mapping LUT**: built by the layer from the shared Layouts and the layer's static modifiers.
- **Effects** (ordered list): write light values into the buffer.
- **Modifiers** (ordered list): transform the LUT or light values.

A layer can have **multiple effects**. Each effect writes to the buffer sequentially in its listed order, overwriting or adding to the previous — so the effects stack (a base-color effect followed by a sparkle effect).

A layer applies **all its enabled modifiers as a chain** during the mapping build (`Layer::rebuildLUT`): each modifier is a coordinate fold, and they compose in child order (M₁∘M₂∘…). Modifiers are **reorderable** in the UI, and order is meaningful (a multiply-then-checkerboard mask differs from checkerboard-then-multiply, just as mirror-then-rotate differs from rotate-then-mirror). The fold contract (the three hooks, the physical→logical build, the live pass) is documented in [ModifierBase](moonmodules/light/moxygen/ModifierBase.md).

Each layer references the shared Layouts. The layer builds its mapping by walking the Layouts container's **physical** coordinates and folding each through the static modifier chain to its logical cell — N physical lights folding onto one logical cell is the fan-out (a Multiply kaleidoscope), so the build never produces a fan-out overflow. Different layers in Effects can have different modifiers, producing different mappings from the same Layouts.

## Effects

Effects produce light colors. They write into the Layer's buffer, which represents a logical grid. The Layer determines the buffer's dimensions (width, height, depth) from the Layouts and its modifiers. Effects receive these logical dimensions and elapsed time (millis) as their rendering context. They compute light positions from the buffer index (e.g. `x = i % width`, `y = i / width`).

Effects use elapsed time for animation, not frame count. Animation speed becomes frame-rate independent: an effect looks the same at 30 fps and 60 fps. This is also what makes the cross-device clock sync work: a shared elapsed-time base means synced visuals across controllers (see [§ Multi-device sync](#multi-device-sync)).

Effects know nothing about hardware, protocols, physical LED layout, or mapping. They only see the logical grid the layer provides.

**Speed convention.** Effects with a speed control use BPM (beats per minute). `uint8_t`, default 60 (= 1 beat per second). Human-readable, musically meaningful, DMX-compatible. The effect converts BPM to animation rate internally using elapsed millis.

### Buffer persistence — the layer does not clear each frame

The Layer's buffer **persists** frame to frame: `Layer::tick()` does not clear it before running effects (the decision, and why not clear-each-frame, is [ADR-0003](adr/0003-layer-buffer-persists-frame-to-frame.md)). It is zeroed once on allocation/resize, and once more in `Layer::prepare()` after `rebuildLUT()`, so a rebuild starts from black and persistence then holds between frames. Each effect owns its background:

- A **full-grid** effect (Plasma, Rainbow, Fire, Noise) writes every pixel each frame.
- A **trail** effect calls `layer()->fadeToBlackBy(amt)` to decay the previous frame, so a comet leaves a fading tail.
- A **read-prior** effect (FreqMatrix scroll, Game-of-Life, a blur) reads last frame's pixels via `draw::get` / `draw::blur`; the persistence *is* its state.
- A **sparse** effect that wants a clean frame calls `draw::fill(buf, {0,0,0})` itself (e.g. RubiksCube).

Fade is a Layer operation: effects register an amount, the Layer keeps the MIN across them and applies one whole-buffer pass at the next frame's start, so N fading effects cost one pass, not N, and never darken each other's fresh pixels.

### Dimensionality

Every effect declares its native dimensionality through `EffectBase::dimensions()`, returning `Dim::D1`, `Dim::D2`, or `Dim::D3` (default: "I iterate every axis the layer gives me"). The Layer uses this to **extrude** lower-dimensional output across the unused axes after each effect's `tick()`:

- **D1**: the effect writes only the column at `(x=0, z=0)` — **1D runs along Y**. Layer copies that column across every other x in z=0, then copies z=0 across every z.
- **D2**: the effect writes only the z=0 slice (the front `(x, y)` face). Layer copies z=0 across every z.
- **D3**: the effect writes every axis itself. Extrude is a one-comparison no-op.

D1/D2 are **opt-in promises**: declaring them tells the framework it can fill the missing axes, saving the per-effect work of iterating z (or x and z). Effects that don't make that promise stay at the D3 default and iterate the whole buffer.

**Why 1D runs along Y, and the unified expand rule.** A lower-D effect occupies the low axes and the framework expands across the next: **1D → 2D adds columns across X**, **2D → 3D adds slices across Z**. 1D-along-Y (shared with MoonLight) makes a 1D effect the natural first column of its 2D form, so expanding to a panel is just "repeat the column," same math (1D-along-X would make it a row expanding downward, a worse fit since a strip is a column). A 1D effect therefore renders correctly on a `1 × N` grid (width 1, height N), but on `N × 1` the extrude runs the wrong way and flattens it. How a physical output (a strip, a row of [Hue lights](moonmodules/light/moxygen/HueDriver.md)) maps to `1 × N` is a layout concern.

Hot-path cost: extrude pays one comparison and returns for the D3 case. For D1/D2 on a layer whose unused axes are size 1 (a D2 effect on a 2D layer, a D1 effect on a 1D `1 × N` layer) the inner loops are guarded by `depth_ > 1` / `width_ > 1` and never run. Real `memcpy` work happens only for a D1 or D2 effect on a layer with more dimensions than the effect writes: exactly the case where you wanted the framework to do the duplication.

Each effect's `dimensions()` is a claim about which axes its loop iterates, not which axes its math could in principle vary along. A "D2 fire" can in future be promoted to D3 by adding z-aware heat propagation; until then declaring it D2 honestly describes what the loop does today.

The `dim` int is also emitted in `/api/types` so the UI derives the dimensional emoji (📏/🟦/🧊) per module; modules don't put dimensional emoji in their own `tags()` strings.

### Robustness rules

**Effects run at every non-empty grid shape.** Modifiers can reshape the logical grid to any size, so an effect's `tick()` produces a correct result for any `(width, height, depth)` of at least one light — a 1×1, a strip, a tall column, a cube. The empty case is the Layer's: `Layer::tick()` skips the effect pass entirely when an extent is 0 or the buffer holds no lights, so that check lives in one place for all effects rather than at the top of each.

**The Layer decides whether a frame runs; the effect decides what it paints.** The modifier pass still runs when the effect pass is skipped: a beat-driven modifier advances its per-frame state through the empty interval, so the chain is in the right phase when the grid returns. An effect owns the checks about *itself*, and returns early for:

- **Its own resources**: `if (!heat_) return;` — a ScratchBuffer it allocated.
- **Its own controls and timing**: `if (speed == 0) return;`, a rate limiter, a divide-by-zero guard on a control value.
- **Producer input**: `if (!f) return;` — no audio frame to react to.

The test: *would the Layer know to skip this?* If yes (an empty grid, a disabled module), it belongs to the Layer. If no (this effect's buffer, this effect's control), it belongs to the effect.

**Effects render at every channel count.** An effect writes per channel, the way `draw::pixel` does (`if (write >= 1) …r; if (write >= 2) …g;`), so a light carries as much of the color as it has channels — RGB on three, R+G on two, R on one. Channels the effect doesn't set belong to the driver. Every light has at least one channel: `Layer::setChannelsPerLight` enforces that at the setter.

**Effects must animate at every tick rate.** Per-tick phase math computed as `dt * bpm * K / 60000` truncates to 0 on devices where `dt < 234/bpm` ms: desktop ticks every 0–1 ms, so even bpm=60 freezes. The fix is to keep the raw `dt * bpm` numerator in the phase accumulator and divide only at the read site:

```cpp
phase_num_ += static_cast<uint64_t>(dt) * bpm;
uint8_t t = static_cast<uint8_t>((phase_num_ * 256) / 60000);
```

See NoiseEffect / MetaballsEffect for the canonical pattern. Animation speed must depend only on `bpm` and wallclock, not on tick rate or grid size.

**Everything that changes over time is driven by elapsed time, never by the frame count.** The rule above is one half of it — a phase that truncates to zero and freezes. The other half is the mirror image and just as wrong: state advanced by a fixed amount *per frame* runs at whatever speed the hardware happens to render. The same gravity setting is an explosion on a desktop at 5,000 fps and a drift on an ESP32 at 470. This applies to every per-frame quantity, not just phase: a force, a velocity, a trail fade, a decay, a drop rate, a simulation step. The user sets a speed; the hardware must not get a vote.

**A faster device renders the same motion more smoothly, not more motion.** The tempting fix — quantise to a fixed 60 Hz and skip the frames in between — is wrong here, because it discards exactly the smoothness the extra frames were rendered for. Instead scale the work by the fraction of a reference frame that actually elapsed, so a device rendering ten times as fast takes ten steps a tenth the size: the same trajectory at ten times the resolution. `particles::FrameTime` is the shared implementation (8.8 fixed point, 256 = one reference frame, whose rate is the constructor's `referenceHz` — 60 by default). It carries the undivided numerator and divides late, for the same reason `BeatPhase` does: one unit is a fraction of a millisecond, so a remainder held in whole milliseconds cannot represent it and the truncated time — which differs by render rate — becomes a framerate dependency of its own.

The check is mechanical: **run the effect at two very different framerates over the same span of simulated time and compare.** If the result differs, something is counting frames. `unit_Effects_framerate.cpp` runs exactly that sweep over every registered effect at 60 and 1200 fps.

**It applies to modifiers too, and to anything else on the tick path.** A modifier that scrolls, rotates or animates its fold is state advanced per call, so the same rule holds: a scroll driven by a per-tick increment moves at the render rate. Anything whose output changes between two ticks with identical inputs is animating and owes elapsed time; a modifier that only folds coordinates from its controls is a pure function and owes nothing.

**Where the machinery lives, and why it is not in the effects.** A trail fade is the case with the most callers, so it is the worked example: `Layer::fadeToBlackBy` takes a RATE per reference frame and the Layer scales it once, for every effect at once. An effect carrying that conversion itself drifts: a version that carries the fraction and a version that floors to 1 apply different decay at the same rate, which is the duplication the one-home rule exists to prevent. Every amount is a rate, with no exception: an effect that wants the buffer blank NOW calls `draw::fill`, since a clear is not a fast fade. Giving 255 a second meaning put a discontinuity in kind at the top of six user-facing fade sliders.

Two traps worth naming. A quantity already gated by wallclock must not ALSO be scaled: an effect that requests its fade only on stepping frames has the Layer scale each request again, throttling it twice. And a COMPOUNDING spatial operation is not a rate: `draw::blur` applied twice at half strength is not one blur at full strength, so the carry pattern that fits a fade does not transfer to it.

**An effect renders a pattern; it does not transform geometry.** When migrating or adding an effect, strip out anything that is really a *modifier* (mirroring, tiling, rotation, scrolling/offset, a kaleidoscope fold, masking, any remap of *where* pixels land), and add it as a separate [modifier](#modifiers) instead. WLED (and other sources we port from) routinely fold these into the effect's own loop (a "mirror" checkbox, a "2D" rotation, a built-in pinwheel), because WLED has no modifier concept; we do. Keeping them out of the effect is what lets any effect compose with any modifier (the same RotateModifier rotates Fire, Noise, or a network-received frame) instead of every effect re-implementing its own half-baked mirror. The test: an effect's `tick()` should only *write colors into the logical buffer for its own coordinates*; if it's reading or rewriting positions to move/fold/duplicate the image, that behavior belongs in a modifier. (This is the light-domain face of *Complexity lives in core; domain modules stay simple*: geometry transforms are the modifier's job, shared once, not duplicated into every effect.)

## MoonLive: the live-script engine

MoonLive lets you author an effect (later: a layout, modifier, driver, or core rule) as **text** and run it on a running device, with no recompile-and-flash cycle. Its standout property is *how* it runs the script: not a bytecode interpreter, but a **native-codegen compiler** — source text is lexed, parsed, lowered to a typed IR, and assembled to real machine code that the render loop calls through a plain function pointer, so a scripted effect runs at near-hand-written speed in the hot path. This is the core construct; a scripted effect (`MoonLiveEffect`) is the thin binding that gives it the MoonModule lifecycle.

The engine is a **domain-neutral core** with one narrow seam, structured as three tiers so adding a CPU is additive, never a rewrite:

- **Front-end** (`src/core/moonlive/`, platform-independent): a recursive-descent lexer + parser over an expression grammar (every function argument is a literal or a nested call) that lowers each statement to a typed **IR** — a flat list of three-address ops over virtual registers. The IR is the seam: it knows *operations*, never an ISA and never a domain. It is compile-time only — consumed during lowering and discarded, so it costs nothing at run time; the CPU executes only the final native instructions.
- **Host builtin table** (the domain seam): the core owns no function names. A *host* registers `{name → descriptor}` — `setRGB`/`fill`/`random16` for LEDs (`src/light/moonlive/`), something else for a display or sensor. A descriptor is either a `Call` (a generic call to a host C function pointer — a pure helper like `random16`) or an `Inline` op (a neutral opcode tag the backend emits inline — a buffer writer, no per-pixel call). This is the ESPLiveScript / ARTI bound-function model; it is what keeps the core LED-free while the hot path stays inline. The LED *names* and the "an element is 3 RGB bytes" meaning live only in the light-domain registration and the per-ISA lowering, never in core.
- **Per-ISA backend** (`src/platform/`, behind the boundary): a tiny named-instruction MacroAssembler (the textbook V8 / LLVM / asmjit shape — append one instruction, back-patch label offsets) plus the IR→bytes lowering that drives it. Xtensa (classic ESP32 / S3), RISC-V (P4), and the host ISA (desktop arm64/x86-64) each are *a new backend file behind the unchanged IR* — the front-end and IR never branch on ISA. Emitted code goes into an `allocExec` block (see [§ Platform abstraction](#platform-abstraction)) and is called each tick.

A recompile is the normal cold-path rebuild: editing the `source` control routes through the same `prepare()` sweep every control change uses, so a new script swaps in live (no reboot), and a parse error surfaces in the module status while the layer renders dark — robust to any input. The module contract is [MoonLiveEffect](moonmodules/light/MoonLiveEffect.md).

**A scripted module differs from a compiled one in one thing only: where its behavior comes from.** Everything else is the same mechanism: the same base class, the same `prepare()`/`release()` lifecycle, the same controls, the same status and memory reporting, the same container contract. A `MoonLiveLayout` is a `LayoutBase` that answers `lightCount()` and `placeLights()` like any other; it just answers them by running compiled machine code instead of arithmetic over its members. When a scripted binding needs a mechanism its compiled sibling does not, that is a finding: either the mechanism belongs in the base for everyone, or the divergence needs its reason stated where it is introduced. A binding that drifts into its own lifecycle stops being a module and becomes a second system to maintain.

The one place this is not yet clean: `applyState()` prepares parent-before-child, so a container asks its children for their extent before those children have prepared. A compiled layout computes its count from its members and does not notice; a scripted one has nothing to answer with until it compiles, so it compiles on demand from a `const` method — the `const_cast` and `mutable` members in `MoonLiveLayout` exist for that and for nothing else. Removing them means giving core a way for children to prepare before a container aggregates them, which is a lifecycle change for every module.

## Modifiers

A modifier (MoonModule) lives inside a layer alongside its effects. Modifiers expose a virtual interface: the Layer calls modifier methods without knowing the concrete type (no `dynamic_cast`). A layer applies **all** its enabled modifiers as a chain, in child order — each a coordinate fold composed into one mapping (see [§ Effects and Layer](#effects-and-layer)).

A modifier is a coordinate transform, applied in one of two ways (the fold contract is in [ModifierBase](moonmodules/light/moxygen/ModifierBase.md)):

- **Static** (`modifyLogicalSize` + `modifyLogical`): folded into the mapping during the cold-path build, so it costs nothing per frame (Region crop, Multiply tile/mirror, a mask).
- **Live** (`modifyLive`): a per-frame coordinate remap for animation (rotation), run only when an enabled modifier needs it — a static-only chain pays nothing.

**Dimensionality** for modifiers defaults to `Dim::D3` (assumed to work in all three axes unless declared otherwise). Unlike for effects, this is purely advisory: the Layer doesn't extrude modifier output. It exists so the UI can render the 📏/🟦/🧊 chip on the card. **MultiplyModifier** is D3 (it has independent multiplyX/Y/Z + mirrorX/Y/Z toggles).

## Mapping and blending

The blend+map step walks each layer in turn: reads each logical light, uses that layer's LUT to find the physical position(s), blends the color into the physical output buffer. This is where logical space meets physical space.

Each mapping LUT is a flat, contiguous lookup table allocated outside the hot path. It is built in `Layer::prepare()` and rebuilt whenever a Layout or Modifier control changes (the controls' `affectsPrepare` returns true) or a Modifier/Layout child is added/removed/replaced/moved; both triggers flow through the same core mechanism, see [§ Event triggering between modules](#event-triggering-between-modules).

The LUT supports four mapping types:

- **1:1 identical**: logical index equals physical index. No table needed (`hasLUT()` returns false, `setIdentity()` mode). Grid without serpentine, no modifiers.
- **1:1 shuffled**: logical maps to one physical, but reordered. Table needed. Grid with serpentine.
- **1:0 unmapped**: logical light has no physical output. Table needed. Sparse layouts (wheel).
- **1:N multimap**: logical maps to multiple physical positions. Table needed (CSR format). Mirror / clone modifier.

Because mapping and blending happen in a single pass over each layer, there is no intermediate "mapped but unblended" buffer. The physical buffer is the only output-side allocation.

## Drivers

**Drivers** (a MoonModule) is the top-level container for one or more drivers. It is the consumer side of the pipeline. The Drivers container owns a shared output buffer and performs blend+map from every layer's buffer into it each frame. Individual drivers then read from this buffer to push to hardware / network.

The shared output buffer is necessary when blend+map writes to arbitrary physical positions via the LUT: the output is not filled sequentially, so a driver cannot read chunk-by-chunk until the full buffer is populated. It is *not* needed for the single-layer, no-blend case (identity or serpentine-shuffle mapping): there a driver can fuse map + output correction + protocol encode into one pass straight into its own output (DMA buffer / packet), skipping the shared buffer.

Each driver (a MoonModule) speaks one protocol:

- **LED drivers**: WS2812 via RMT (multi-pin), plus one DMA-driven parallel driver ([ParallelLedDriver](moonmodules/light/drivers.md#parallelled)) whose `peripheral` control picks the bus backend the chip supports — the `i80` bus (LCD_CAM on the S3/P4/S31, and the classic ESP32's I2S peripheral in i80 mode, which is the classic's only >8-lane parallel route — IDF's `esp_lcd` picks the backend per chip), our own-GDMA MoonI80 (LCD_CAM, adds the streaming ring + 74HCT595 expander), or the P4's Parlio. All are DMA-driven and behind the platform boundary; the driver rounds an i80 bus up around whatever pin count is configured (any count from 1) and parks unused lanes on a pin already driven.
- **DMX / ArtNet**: sends DMX over UDP. Supports addressable LEDs and conventional DMX fixtures (pars, moving heads, dimmers).
- **Preview**: streams light data to the web UI via WebSocket.
- **Desktop output**: SDL2 or terminal for visual preview. Desktop also serves as a high-speed processing node, driving lights via ArtNet/DDP over the network.

Each driver child reads from the Drivers container's output buffer. Everything before the Drivers container is platform-independent.

**Output correction** turns logical RGB into the physical signal: **brightness** scaling, channel **reorder** (RGB→GRB via a *light preset*), and **white** derivation for RGBW. The Drivers container owns the global `brightness`; each driver picks its own light preset (its `preset` control) and applies the correction per-light into its own buffer/packet, so two strips on one device can be wired differently. Preview is exempt (it shows the raw logical buffer). The brightness LUT rebuilds on the cheap `onControlChanged` tier ([§ Event triggering](#event-triggering-between-modules)), so the slider stays fluent.

**An effect drives a fixture's non-color channels through role setters.** A light is as wide as its fixture ([§ Buffer types](#buffer-types)), with color at offset 0 and the fixture's other roles wherever its light preset puts them. `setPan()` / `setTilt()` / `setZoom()` write those, and each is a **no-op when the fixture has no such channel**, so one effect is valid on a moving head and on a strip alike: on the strip the pan write does not land and the effect just paints color.

Two rules separate those channels from color, and both matter:

- **Brightness never scales them.** Brightness is a light-output setting; scaling pan by it would swing a moving head toward 0/0 as the rig dims.
- **They interpolate but never accumulate** (the rule; the additive half is NOT yet implemented, see below). A blend op that INTERPOLATES (opacity, a crossfade) is meaningful on any channel, and on pan it is a genuine feature: the head sweeps smoothly from the old aim to the new one as a layer fades in. A blend op that ACCUMULATES (additive) is meaningful only on emissive channels, where summing two lights models two sources lighting one surface. Summing two aims models nothing, since it points at neither and saturates at hard-over as soon as both layers are positioned, so an accumulating op should fall back to assignment on a motion channel with the topmost writer winning. **Today `blendMap` treats a light as opaque bytes and adds motion channels along with color**; it only bites with two enabled layers on a fixture that carries motion, and the fix is [backlogged](backlog/backlog-light.md).

**DMX fixtures are addressed as a daisy chain of IDENTICAL fixtures**, the same model addressable LEDs already impose: a strip is N identical pixels at a fixed stride, and a DMX run is treated as N identical fixtures at a fixed stride. One light preset describes one fixture, its channel count is the stride, and fixture *n* starts at `start + n x channelCount`. Twenty-five channels per fixture puts them at DMX 1, 26, 51, and so on, and the driver's `count` says how many are on the chain.

This is what makes a moving head reachable by the same pipeline as a pixel: the light domain produces one logical light per fixture, and the driver expands each into that fixture's channel block through the preset. It is also the cheapest thing to configure, since only the start address and the fixture type are needed, never a per-fixture address table.

The trade is deliberate: **a chain must be homogeneous**. Mixing fixture types on one universe, or leaving gaps between fixtures, has no expression in this model, and neither does a fixture whose address does not sit on the stride. Those need a per-fixture address map, which is the fixture-model work ([backlog](backlog/backlog-light.md)); until then, a mixed rig is served by giving each fixture type its own driver instance with its own preset, start address and count.

### Multicast and IGMP snooping

Three things projectMM sends to more than one listener, and they do not all use the same transport, because the protocol's owner decides it and not us:

| | transport | why |
|---|---|---|
| WLED audio sync | multicast `239.0.0.1` | WLED's usermod both sends and receives there, never on broadcast |
| Device discovery | multicast `239.255.77.77`, plus broadcast when `wledCompatible` | WLED apps browse the discovery port on broadcast |
| E1.31 / sACN output | unicast by default, multicast opt-in | multicast is the spec's native mode, but see below |

**Broadcast** reaches every device on the subnet. Each one takes the interrupt, walks up the stack, finds nothing listening on the port and discards the packet. At LED frame rates that is real work imposed on every phone, laptop and printer on the LAN.

**Multicast** is addressed to a group, and only the devices that joined it (via IGMP) accept the packet. The rest never see it, which is what makes it the better neighbour in principle.

**In principle**, because the win depends on the switch. A switch with **IGMP snooping** watches those join messages and learns which of its ports actually want the group, then forwards the traffic only there: the saving is real and happens in hardware. A switch **without** snooping cannot know, so it does the safe thing and floods the group out of every port, exactly like broadcast. WiFi is worse than that: multicast and broadcast alike go out at the lowest basic rate so every station can hear them, which is far slower than a unicast frame to one associated station.

Firmware cannot detect which kind of network it is on. That is why **multicast is never an automatic upgrade here**: sACN multicast and dropping the discovery broadcast are both opt-in choices for someone who knows their switch, and unicast (or broadcast, where a protocol demands it) stays the portable default.

Network-based drivers (ArtNet, E1.31, DDP) pace their output with a **non-blocking elapsed-time gate**, never a blocking wait (no `delay`/`vTaskDelay` — that would stall the single-threaded tick, the hot-path rule). The gate is the `lastSendTime`/`millis()` pattern: `if (now − lastSendTime < interval) return;` early-exits the tick so every other module's loop keeps running, exactly how FPS limiting works (`NetworkSendDriver`, `fps` control). **Frame-rate pacing is required** and implemented this way. **Inter-packet pacing** (spacing the universes within one frame) uses the same non-blocking gate *if* a receiver drops packets under a burst — it is not needed by default (the bench ArtNet matrix test runs clean bursting the universes), so it is added only when a target requires it, never as a busy-wait between packets.

## Memory strategy

All buffers are allocated as single contiguous blocks outside the hot path, at startup or when configuration changes (LED count, layout size, layer count). They are then reused every frame with zero allocations in steady state. Measured per-module timing and memory for each platform: [performance.md](performance.md).

### Pay for what you use

A module holds heap **only for capabilities it is actually exercising**, the same zero-overhead principle C++ applies to abstractions ("you don't pay for what you don't use"). Concretely, for every module:

- **A module not in the tree costs nothing.** Modules are heap-allocated through `MoonModule::operator new` when added (via the factory or boot wiring), so a deviceModel that omits a module pays zero — not even its `classSize()`. This is the base case the rest of the rule extends inward.
- **A feature's buffer allocates on first use, not at `setup()`.** When a module *is* present but a given capability is dormant (a driver with no output attached, an MQTT client with HA discovery toggled off), that capability's buffer is `nullptr` until the code path that needs it runs. Allocating eagerly at `setup()` for a path that may never execute is the anti-pattern this rule forbids — it charges every instance for the worst case.
- **The allocation frees in `release()`** (and on the transition that makes the capability dormant again — a disable, a toggle-off), and is reported through `dynamicBytes()` so `/api/system` and the memory scenarios see the real ladder. `MoonModule::release()` reverse-recurses into children, so a subtree's memory unwinds bottom-up with no leak.

The result is a memory ladder that tracks configuration exactly: module-absent → 0; module-present-but-feature-off → just the class instance; feature-active → `+dynamicBytes()`. The LED driver's output buffer and the MQTT module's discovery-config scratch are the worked examples; the rule governs every module. It matters most on a no-PSRAM ESP32, where the internal-heap reserve (`HEAP_RESERVE`) is the tightest constraint, so a buffer held but unused spends the reserve the render loop, WiFi, and HTTP depend on.

### Buffer types

- **Layer buffers**: one per active layer, holds the logical light data for one effect chain. Allocated in PSRAM when available. On memory-constrained devices, consumers may read from the layer buffer directly (no mapping, no blending, no physical buffer needed).
- **Physical buffer**: when present, holds the blended+mapped output. It is a *blend* buffer, needed only for compositing (>1 layer, or any alpha/additive blend); it is not what provides producer/consumer parallelism. Under the [two-core handover](#parallelism), parallelism comes from the consumer's own working copy, the encoded DMA buffer for a clockless LED driver, or the kernel socket buffer for ArtNet, which decouples the producer (filling the next Layer frame) from the consumer (transmitting the previous one).
- **Mapping LUT**: flat lookup table for logical→physical. Read-only during rendering. PSRAM is fine: sequential reads are cache-friendly.

All buffers are raw `uint8_t*` arrays sized `channelsPerLight * nrOfLights`. There is no pre-allocated per-channel array and no fixed channel layout: `channelsPerLight` is a runtime value (a `uint8_t`, so 1–255), so RGB (3), RGBW (4), and multi-channel DMX fixtures all use the same code path; the buffer simply gets wider. Channel layout is configured via offsets (see MoonLight's [LightsHeader](https://github.com/ewowi/MoonLight/blob/main/src/MoonLight/Layers/LightsHeader.h) pattern).

Network input (ArtNet receive, WebSocket) is processed synchronously at a defined point in the frame loop. Zero extra buffers, no race conditions. The trade-off is up to one frame of latency (~16 ms at 60 fps), imperceptible for LEDs.

### Adaptive allocation

The system checks available heap before each allocation and degrades gracefully when memory is insufficient (the allocate-on-demand-with-a-cascade decision, over fixed buffers, is [ADR-0002](adr/0002-adaptive-memory-degradation-cascade.md)). A minimum reserve (`HEAP_RESERVE = 32 KB`) is kept for stack, HTTP, WiFi, and overhead.

- **Mapping LUT** is created only if all of: modifiers exist on the layer; layout is not a simple non-serpentine grid (where physical == logical); enough heap available after the reserve.
- **Driver output buffer** (see [§ Drivers](#drivers) for what it's for) is created only when the pipeline must write into physical space rather than hand a driver a layer's logical buffer directly — that is, when **two or more layers are enabled** (they must be composited into one buffer) **or** a layer has a **mapping LUT** actually allocated (logical≠physical) — and enough heap is available. A single enabled layer with no LUT needs no output buffer: drivers read its buffer directly (the zero-copy fast path).

### Degradation cascade

Best to worst:

1. **Full pipeline**: LUT + driver output buffer. Modifier applied, clean separation.
2. **Skip LUT + driver buffer**: modifier not applied, forced 1:1 mapping. No intermediate buffers. (A LUT without a driver buffer to map into is useless; they're always skipped together.)
3. **Reduce layer dimensions**: halve width/height until the buffer fits, minimum 8×8.

Each degradation is observable via `lutSkipped()` and reported in `/api/system` per-module metrics.

### Invariants

Non-negotiable:

- Effects always write to their layer's logical buffer. Never to output, never to physical coordinates.
- Drivers always own the output path (blending, mapping, brightness correction, channel reordering).
- Layer buffer is mandatory: if it doesn't fit, reduce dimensions until it does ("at least see something").

### Per-module reporting

Every MoonModule self-reports `classSize()` / `dynamicBytes()` / `tickTimeUs()` (a core base-class feature; see [§ MoonModules](#moonmodules)). For the light pipeline specifically, memory scenarios use those numbers to verify that 1:1 pipelines allocate zero intermediate buffers and that the degradation cascade triggers at the right thresholds.

### Scaling to available memory

| Device | Memory | Typical capability |
|--------|--------|--------------------|
| ESP32 + OPI PSRAM | 2–8 MB | Many layers, 10K+ LEDs |
| ESP32, no PSRAM | ~320 KB internal | Full pipeline: double buffering, mapping, blending, parallelism. Proven up to 16 K lights (128×128 measured live on Olimex; see [performance.md](performance.md)). The degraded path (single Layer, 1:1 direct, no blending) is reserved for installations that grow beyond what the full pipeline fits. |
| Teensy 4.x | 1 MB internal, no PSRAM | Comfortable headroom for several layers; excellent DMA-based LED output (OctoWS2811). Ethernet built-in on 4.1, optional on 4.0. |
| Desktop / RPi | Abundant | No constraints |

The architecture does not assume PSRAM is present. Buffer counts and sizes are determined at runtime based on available memory and reallocated when configuration changes.

## Multi-device sync

How lighting uses the core [multi-device runtime](#multi-device-runtime) (discovery + clock sync) to drive an installation spanning multiple controllers:

- **Synced visuals from the shared clock.** Effects animate off elapsed time ([§ Effects](#effects)), so a synced clock is what makes a wall of controllers animate in lockstep regardless of each one's frame rate. This is the light-domain payoff of the core clock sync.
- **Light distribution**: one device sending rendered light data to another uses the existing ArtNet / E1.31 / DDP standards. The ArtNet *driver* sends to fixtures; device-to-device distribution as a sync topology is filed in [backlog-core](backlog/backlog-core.md). No bespoke protocol.

# Web UI

![UI overview](assets/ui/ui_overview.png)

The UI is a handful of hand-maintained files: `index.html`, `app.js`, `style.css`, plus two focused ES modules `app.js` imports (`preview3d.js` for the WebGL 3D preview, `install-picker.js` shared with the web installer). No frameworks, no build tools, no npm. Served directly by the embedded HTTP server.

The UI is **MoonModule-driven**. It contains no hard-coded knowledge of specific effects, layouts, or drivers. It queries the system for the current MoonModule tree (layers, effects, modifiers, layouts, drivers, each with their controls) and renders generically:

- Each MoonModule shows as a card with its name and declared controls.
- Controls are auto-rendered by type (slider, toggle, color picker, text input, dropdown).
- Modules can be switched (change which effect a layer uses) and linked (assign a layout to a layer).

Adding a new MoonModule with controls needs **zero changes** to the UI files. This extends to the tree-mutation affordances: which modules accept children (and of what role) comes from each type's `acceptsChildRoles()`, and whether a module can be deleted/replaced comes from its `userEditable()`: both declared on the C++ side and reported in `/api/types` + `/api/state`. The UI hardcodes no list of "which types are containers" or "which roles are editable"; a new container type or a fixed child is a one-line C++ override.

The light domain plugs into the UI at three points: a fixed top-level tree (Layouts / Effects / Drivers pinned in `main.cpp`, root reorder disabled while child reorder works via drag-and-drop), a binary WebSocket preview channel ([PreviewDriver](moonmodules/light/moxygen/PreviewDriver.md): a `0x03` coordinate table sent once per LUT rebuild plus per-frame `0x02` RGB point lists, so sparse layouts preview at their real positions), and per-role emoji for the chip filter (the `ROLE_EMOJI` map in `app.js` is the single source of truth: `effect`, `driver`, …, `service`). Full UI spec: [docs/moonmodules/core/ui.md](moonmodules/core/ui.md).

## Tag emoji legend

The legend itself lives with the people who read the chips: [How projectMM works § The emoji on
every card](tutorials/how-projectmm-works.md#5-the-emoji-on-every-card). What belongs here is the
mechanism.

A module's chips come from three sources, rendered identically on the card and the type picker: a
**role** chip and a **dimensional** chip, both UI-derived from `role` and `dim` through the
`ROLE_EMOJI` / `DIM_EMOJI` maps in `app.js` (the single source of truth for those two), and the
curated **`tags()`** string, a flash literal the module returns which the UI splits into grapheme
clusters, one chip each.

**Role and dim are never repeated in `tags()`**: the UI already adds them, and a module that spells
them again gets the chip twice. `tags()` carries origin, creator, and the capability groups the
legend lists. An emoji earns its place by GROUPING several modules, which is what the picker's chip
filter is for: a unique marker per module filters nothing, so a module that fits no group returns
"".

A scripted module answers the same way a compiled one does: `MoonLiveEffect::tags()` returns what
the loaded script's `string tags()` declared, so a script's row reads like any other.
