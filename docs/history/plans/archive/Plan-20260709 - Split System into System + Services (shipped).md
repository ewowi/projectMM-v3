# Plan — Split System into System + Services (System Modules vs Service Modules)

## Context

Today `ModuleRole::Peripheral` conflates two categories that both parent under **System**: the genuinely **user-added capability bridges** (Audio, IR — optional, add/delete), and **fixed things that borrow the role only to render a delete button** (TasksModule was given `Peripheral`+delete for exactly that; I2cScan and FileManager carry it while being always-there). Network's own children (MQTT, Devices) are a third, separate thing — always-there infra, wired-by-code, never user-added. The design note [docs/backlog/system-modules.md](../../../backlog/system-modules.md) settles the split:

- **System Modules** — fixed, wired-by-code, no add/delete: System's vitals + the fixed inspection modules (Tasks, I2cScan; later Memory, Pins) + always-there infra (Network, Firmware, Improv).
- **Service Modules** (a new top-level **Services** container) — user-added, add/delete/replace, `ModuleRole::Service`: Audio, IR. (I2cScan → a fixed System Module — it inspects this-device hardware; MQTT/Improv/Devices stay code-wired — see §3.)

The unifying insight (the justification, *Common patterns first*): **Services : System :: Layers/Drivers : the light pipeline.** projectMM already has the "top-level container holding user-added children of one role" pattern (`Layers` holds effects, `Drivers` holds drivers); `Services` is that exact shape applied to the core domain. So this isn't a new mechanism — it's the existing container pattern reused, and `Services` is modelled directly on `Layers`/`Drivers`.

## Decisions locked (PO, in the design note)

- Container name **Services**; role **`ModuleRole::Peripheral` → `Service`** (name = role, one concept — chosen on merits, not precedent).
- System Modules are **FIXED** (wired-by-code, no delete); Service Modules are **user-managed**.
- **FileManager** stays a standalone top-level module — just drop its incidental Peripheral role.
- **TasksModule** stops being `Peripheral`+delete; becomes a fixed, wired-by-code System child.
- Docs split **`core/services.md` → `core/system.md` + `core/services.md`**.

## Design

### 1. The role rename (`Peripheral` → `Service`)

`src/core/MoonModule.h`: rename the enum member `ModuleRole::Peripheral` → `Service`, and `roleName()` `"peripheral"` → `"service"`. Update the doc comment ("Peripheral is a module attached to SystemModule…" → "A Service is a user-added module in the Services container that bridges to the outside world — hardware or network"). The user-added Service Modules (Audio, IR) get `role() → Service`. The code-wired ones (MQTT, Devices) also update their probed role to `service` mechanically. I2cScan and Tasks, now fixed System Modules, DROP the role entirely (→ `Generic`/roleless, like Improv — no container claims them, so no add/delete). This is mechanical; the wire value `"service"` is what the UI's `allAcceptedChildRoles()` matches.

### 2. The Services container module

New `src/core/Services.h` (or `ServicesModule`) — a thin container modelled on `Layers`/`Drivers`:
- `acceptsChildRoles() const override { return "service"; }` — so the UI shows add/delete for its children (the `isUserEditableChild` gate, depth>0 + accepted role).
- No controls of its own (like `Layers`); it's a grouping node. Registered in `main.cpp` with docPath `core/services.md#services`, created via factory, added via `scheduler.addModule(services)`.
- Domain-neutral, core.

### 3. Reparent — the user-added *capability* modules only

Only **Audio** and **IR** move under `services` (user-added this-device capability bridges, currently `parent_id: System`, `role()` → `Service`). Everything else is either a fixed System Module or stays put:

- **I2cScan → a fixed System Module, NOT a Service** (PO decision, and it drives a behaviour change — see §4a). It *inspects this device's hardware* (what's on the I²C bus), which is the System Modules' remit alongside Tasks/Memory/Pins — the inspection/bring-up toolkit. So it is **always available, wired-by-code, no add/delete**, not user-added. This overrides its being optional+pin-configured: the deciding criterion is *what it does* (hardware inspection = System), and a System Module is always there.
- **MQTT, Improv** — stay code-wired children of Network, unchanged.
- **DevicesModule — stays a wired-by-code child of Network, UNCHANGED** (PO decision). It's **fleet-scope** — discovers/lists *other* devices, drives Hue, the seed of future multi-device features — so neither a this-device System Module nor a Service Module. Its eventual home is a **later decision** (standalone top-level, or a "Fleet"/"Devices" container once a second fleet module justifies one — *Concrete first*). For this split it stays under Network.

So the reparent is narrow: **Audio / IR `parent_id: System` → `Services`**, role → `Service`. Code-wired modules (MQTT, Improv, Devices) are untouched — they carry the incidental `Peripheral` role only via the factory probe, which §1's rename updates to `service` mechanically without moving them.

### 4. Fixed System Modules (Tasks + I2cScan; later Memory/Pins)

**TasksModule** and **I2cScanModule** both become **wired-by-code children of System** (like Improv under Network): created in `main.cpp`, `systemModule->addChild(...)`, `markWiredByCode()`, no Service role → the UI renders no delete (their role isn't an accepted child role of any container). Drop TasksModule's `role() → Peripheral` and update its `///` (remove the "Peripheral so the UI shows delete" note — the opposite is now true). SystemModule's `acceptsChildRoles()` returns `""` (was `"peripheral"`) — System accepts no *user-added* children now, so its wired-by-code children get no add/delete affordance. That's the mechanism enforcing "System Modules are fixed."

#### 4a. I2cScan becomes always-available (a behaviour change)

Today I2cScan is added per-board via the catalog (only where a board declares an I²C bus, e.g. the S31) with `sda`/`scl` defaults the user overrides. As a fixed System Module it is now **always present on every board**, wired-by-code. Consequences to handle:
- It must **default to a sensible idle state on every chip** — it already reports "set sda / scl pins" until real pins are entered and opens *no* bus at boot, so an always-present I2cScan **costs nothing until you press scan**: no bus opened, no pins driven, until the user scans. (Unlike TasksModule, which *does* sample every `loop1s` once added — I2cScan is fully passive until the scan button, which is the stronger idle property; keep it.)
- Its `sda`/`scl` pins default (GPIO21/22 today) stay as a *starting point*; a board with a fixed bus (the S31's `sda:51,scl:50`) still needs those defaults — but now via System's wired-by-code creation, not a catalog `parent_id` entry. **DECIDED:** the board overrides the I2cScan control VALUE (sda/scl), not the parentage — same as any pin control. GPIO21/22 stays the default; a board with a fixed bus (S31 sda:51/scl:50) injects those as control-value defaults for the always-present I2cScan.
- The catalog entries that currently *add* I2cScan (S31, others) are **removed** — it's no longer user-added; it's always there. Their `sda`/`scl` values become a `System.I2cScan.sda/scl` control default the board injects, if we go that route.

### 5. FileManager

Drop its `role() → Peripheral` (→ `Generic`, or leave roleless). It stays `scheduler.addModule(fileManagerModule)` top-level, wired-by-code — no behaviour change except it no longer claims a container-child role.

### 6. Catalog migration

`web-installer/deviceModels.json`:
- **Audio** catalog entries: `"parent_id": "System"` → `"Services"` (still a user-added Service Module).
- **I2cScan** catalog entries: **removed** — it's now a fixed System Module, always wired-by-code, not catalog-added. Any board-specific bus pins (the S31's `sda:51,scl:50`) migrate to a control-value default the board injects for the always-present `I2cScan` (per §4a), *not* a module-add entry.

`check_devices.py` validates the new parentage + that no removed-type entries linger.

### 7. Persistence migration — **NONE (PO decision: new project, best design now)**

A pre-split device could have Audio/IR saved positionally under `System`'s config file, and — because persistence is positional-per-parent (see Execution notes) — on upgrade the load hits the wired-by-code `break` at the Audio slot and the saved module is dropped rather than re-homed. **DECIDED (PO): no migration.** projectMM has effectively no installed base yet, so back-compat machinery isn't worth its weight; a stale config simply loses its Audio/IR and the user re-adds it (a catalog-provisioned device self-heals on the next installer run, which now writes `parent_id: "Services"`). This is the same "choose the best system now, no back-compat alias/migration" call that drove renaming the module *type strings* (`AudioModule`→`AudioService`) without a factory alias — one consistent decision, not a reboot-to-apply or a bespoke migration shim. If an installed base ever justifies it, the migration seam is `FilesystemModule::migrateRenamedConfigs()` and the approach is a boot-time in-memory subtree move keyed by module type.

### 8. Docs split

`docs/moonmodules/core/services.md` splits:
- **`core/system.md`** — System + its fixed children: Tasks + I2cScan (now), Memory/Pins (later), plus Network/Firmware/Improv/FileManager/Devices.
- **`core/services.md`** — the Service Modules: Audio, IR.

Update every `main.cpp` `registerType(..., "core/services.md#x")` docPath to the correct new file (`system.md#…` for System children, `services.md#…` for Services). `mkdocs.yml` nav gains `system.md`. `check_specs.py` validates each docPath resolves — this is the gate that catches a missed move.

## Files

- **Edit:** `src/core/MoonModule.h` (role rename), `src/core/{AudioModule,IrModule}.h` (role → Service), `src/core/{MqttModule,DevicesModule}.h` (probed role → service), `src/core/I2cScanModule.h` (drop Peripheral role → fixed System child), `src/core/TasksModule.h` (drop Peripheral, become fixed), `src/core/FileManagerModule.h` (drop Peripheral), `src/core/SystemModule.h` (`acceptsChildRoles` → ""), `src/main.cpp` (new Services container + reparenting + Tasks wired-by-code), `web-installer/deviceModels.json` (parent_id), `docs/moonmodules/core/services.md` → split, `mkdocs.yml`. (No persistence-load-path change — §7: no migration.)
- **New:** `src/core/Services.h`, `docs/moonmodules/core/system.md`.
- **Tests:** a unit test that `Services` accepts `service`-role children and System accepts none; that a Service Module renders as user-editable and a System Module does not; update `unit_TasksModule` (no longer Peripheral). (No persistence-migration test — §7: no migration.)

## Verification

1. `cmake --build build` clean; `ctest` + scenarios green; `check_specs.py` green (every docPath resolves to the right split file — the key gate); `check_devices.py` green (catalog parent_id valid); `check_platform_boundary.py`.
2. Live: on a board, Services shows Audio/IR/etc. with add/delete; System shows Tasks/etc. with NO delete; adding/removing a Service works; a persisted device with old parentage still loads (migration).
3. Docs render (`mkdocs --strict`), the two pages resolve, no dead anchors.

## Scope guard

This is the *structural* split only. It does NOT build Memory or Pins (separate specs) — it just makes System the right home for them. It does NOT add the core-affinity/relocate features. Keep the Services container thin (a `Layers`-style grouping node, no controls) — if it starts growing logic, that's a smell.

Save as this file. Mark `(shipped)` when it lands. **Merge order (PO):** fold onto `next-iteration` → grows PR #43 (TasksModule) to cover both.

## Execution notes (code-verified before implementing)

An Explore pass over the shipped code corrected two assumptions this design was written against; the implementation follows these, not the sketch above where they differ:

- **Persistence is positional per-parent — there is NO `parent_id` in saved config.** `FilesystemModule::applyNode` rebuilds each parent's children by index from that parent's own file, so a child always reloads under the same parent. This is what makes a pre-split device's saved Audio/IR (positionally under System) get **dropped** rather than re-homed on upgrade: the load hits the wired-by-code `break` at that slot. Per the PO decision in §7, we **do not migrate** — no reparenting seam is added. (If an installed base ever justified it, the seam would be `FilesystemModule::migrateRenamedConfigs()` and the approach a boot-time in-memory subtree move keyed by module type — but that is explicitly NOT built here.)
- **The UI's `isUserEditableChild` assumes a 1:1 role→container mapping** (it tests `mod.role` against the *union* of all accepted roles). The rename **preserves** this: System drops `peripheral`, Services gains `service`, and the three fixed modules (Tasks/I2cScan/FileManager) drop their role to `Generic` — so every container-accepted role still maps to exactly one container. This means **no `app.js` change** is needed, but it is a correctness constraint: no fixed module may keep a container-accepted role.
- **Five modules return `Peripheral`** (not the two the sketch implies): Audio + IR keep it (→ `Service`); Tasks + I2cScan + **FileManager** drop it (→ `Generic`).
- `check_devices.py` — **no change needed** for `parent_id:"Services"` to resolve: `Services` is factory-registered under that exact name (`registerType<Services>("Services")`), so it's already in the validator's `factory_types` set. `BOOT_WIRED_TYPES` stays `{System, Network, Drivers}` — it's only for names referenced as `parent_id` that the factory does NOT register under that short name (System→SystemModule, Network→NetworkModule). Adding `Services` there would be redundant, so it isn't added. (Separately: the validator never checks `parent_id` against the valid set at all — a pre-existing gap, not this split's to fix; noted for a future validator hardening.)
