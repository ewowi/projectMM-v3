# Core system

The device's fixed infrastructure — identity, network, provisioning, firmware, and the inspection tools. These modules are **always present and wired by code**, not user-added; the user does not add or delete them. User-added capability modules (Audio, IR) live in the `Services` container instead — see [core/services.md](services.md). Every row links to its generated technical page (the full API, from the `.h`) and its tests. Cross-cutting rationale that no single `.h` owns lives in the prose sections below the table.

## System modules

<a id="system"></a>

### System

The device's identity and vitals — name (behind mDNS `<name>.local`, the SoftAP SSID, the DHCP hostname), uptime, heap, and per-module footprint reporting. Its fixed inspection children (Tasks, I2C scan) hang beneath it.

<img src="../../assets/core/SystemModule.png" width="300" alt="System module controls">

- `deviceName` — the device identity behind mDNS `<name>.local`, the SoftAP SSID, and the DHCP hostname.
- `deviceModel` — the board model (drives the installer catalog entry).
- `expertMode` — reveals advanced tuning/diagnostic controls (marked 🔧) across the UI; off by default.
- `logLevel` — serial verbosity (None/Error/Warn/Info/Debug/Verbose); default Warn silences the periodic tick line but keeps warnings/errors. First 60 s always logs at Info.
- read-only vitals — `uptime`, `fps`, `heap`, `psram`, `flash`, `chip`, and per-module footprint.

Detail: [technical](moxygen/SystemModule.md)

[Tests](../../tests/unit-tests.md#systemmodule)

<a id="network"></a>

### Network

WiFi / Ethernet connectivity, static-IP configuration, RSSI and TX-power reporting. Brings the device onto the LAN before the HTTP and WebSocket servers start.

<img src="../../assets/core/NetworkModule.png" width="300" alt="Network module controls">

- `mode` — WiFi / Ethernet / off.
- `ssid` / `password` — WiFi credentials.
- `mDNS` — the `<name>.local` hostname.
- `addressing` — DHCP or static; static exposes IP / gateway / subnet / DNS fields.
- `ethType` / `ethPhyAddr` / `ethRstGpio` / … — Ethernet PHY configuration.
- read-only — `rssi` (dBm), `txPower` (dBm).

Detail: [technical](moxygen/NetworkModule.md)

[Tests](../../tests/unit-tests.md#networkmodule)

<a id="improv-provisioning"></a>

### Improv provisioning

Serial/BLE Improv Wi-Fi provisioning: the web installer hands credentials to a fresh device over this protocol during the flash-and-connect flow. [Improv Wi-Fi](https://github.com/improv-wifi) is an open standard, and its [sdk-cpp](https://github.com/improv-wifi/sdk-cpp) / [sdk-js](https://github.com/improv-wifi/sdk-js) are the specification this implements, so any Improv-capable installer can provision a projectMM device.

<img src="../../assets/core/ImprovProvisioningModule.png" width="300" alt="Improv provisioning module controls">

- `provision_status` — read-only provisioning state.

Detail: [technical](moxygen/ImprovProvisioningModule.md)

<a id="devices"></a>

### Devices

Discovers and lists other projectMM devices on the LAN (the `devices` List control), each row expanding to a detail panel; persists the last-known list across reboot. Fleet-scope (it looks at *other* devices), a wired-by-code child of Network.

<img src="../../assets/core/DevicesModule.png" width="300" alt="Devices module — discovered LAN devices">

- `devices` — a List control of discovered devices; each row expands to a detail panel. Persistable.
- `wledCompatible` — announce on WLED's broadcast address as well as the multicast group
  (**default off**). WLED apps and devices browse the discovery port on **broadcast**, so a
  projectMM device does not appear in them until this is turned on. Off is the better neighbour
  on the network: a broadcast wakes every phone, printer and laptop on the LAN to parse a packet
  none of them want. See [multicast and IGMP snooping](../../architecture.md#multicast-and-igmp-snooping)
  for when that actually saves traffic.

Presence always goes to the projectMM group `239.255.77.77`, so peers find each other however
this control is set; `wledCompatible` only adds the broadcast copy.

Detail: [technical](moxygen/DevicesModule.md)

[Tests](../../tests/unit-tests.md#devicesmodule)

<a id="mqtt"></a>

### MQTT

Bridges the light to an MQTT broker so a home-automation hub (Homebridge) can control it — a transport over the shared `Scheduler::setControl` apply-core, not new control logic. Our own dependency-free MQTT 3.1.1 client; disabled until a broker is set. Always-there network infra, a wired-by-code child of Network. Topics, color-wheel mapping, and the Homebridge config: ⌄ details.

<img src="../../assets/core/MqttModule.png" width="300" alt="MQTT module controls">

- `broker` — the broker hostname (e.g. `homeassistant.lan`) or IP. A hostname is resolved via DNS.
- `port` — broker port (default 1883).
- `username` / `password` — broker credentials (optional; the password is stored obfuscated like the WiFi password).
- `haDiscovery` — announce a Home Assistant MQTT-discovery light (default off, opt-in). HA already auto-discovers the device over the WLED `/json` shim (color + palette + sensors, no broker), so this stays off to avoid a duplicate entity; turn it on for broker-only / cross-subnet setups where mDNS doesn't reach. When on, HA auto-creates a wired entity; toggling it off removes it. See the [home-automation guide](../../usecases/home-automation.md).
- read-only — `mqtt_status` (`disabled` / `idle` / `connecting` / `connected` / `disconnected` / an error).

Detail: [technical](moxygen/MqttModule.md)

[Tests](../../tests/unit-tests.md#mqttmodule)

<a id="firmware-update"></a>

### Firmware update

Over-the-air firmware flashing — the one operation that swaps the binary and needs a power cycle (every *config* change applies live; a firmware OTA does not).

<img src="../../assets/core/FirmwareUpdateModule.png" width="300" alt="Firmware update module controls">

- `firmware` — the OTA image to flash.
- read-only: `version`, `build`, `partition`. Where a device carries two images, `image` selects
  which one those describe and which one an install writes: the app it runs, or MoonBase in the
  factory slot. Its presence is also what tells the UI that installs run through the
  reboot-into-MoonBase cycle, behind one "updating firmware" overlay, and that a **Restart in
  MoonBase** button belongs on the card
  ([architecture.md § MoonBase](../../architecture.md#moonbase-the-second-boot-image)).

Detail: [technical](moxygen/FirmwareUpdateModule.md)

[Tests](../../tests/unit-tests.md#firmwareupdatemodule)

<a id="file-manager"></a>

### File Manager

A boot-wired system tool (distinct from Filesystem, the persistence *engine*): browse and manage the device filesystem from a dedicated panel — a lazy expand/collapse folder tree (VS Code / Explorer shape) plus an inline text editor. Browsing is UI-side over `/api/dir` + `/api/file`, so the module itself stays minimal. Tree/toolbar/editor behaviour: ⌄ details.

<img src="../../assets/core/FileManagerModule.png" width="300" alt="File Manager panel — folder tree + toolbar">

- `file browser`, the panel itself: an expand/collapse folder tree with a toolbar (＋folder / ＋file / upload / backup / restore / delete / refresh) and an inline text editor. The module's main surface (⌄ details for the interactions).
- **Backup (⤓)**, download the device's files (config, scripts, presets) as one `.json` bundle: every successfully read file, byte-verified against the directory listing; an unreadable or non-text file is skipped and named, and only a verified-short read aborts the backup. **Keep the file private: it contains the WiFi password.** For a device on firmware from before this button, the [installer page](https://moonmodules.org/projectMM/install/) offers the same backup as a bookmarklet.
- **Restore (⟲)**, upload a backup bundle (press twice: it overwrites the device's files). Known renames from [MIGRATING.md](../../MIGRATING.md) apply in the browser before upload, then a report lists everything that needs an eye: renamed and mapped entries, values to review, module types or controls this firmware no longer has (per [ADR-0013](../../adr/0013-no-migration-code-robust-persistence-plus-documented-breaks.md) the device itself never migrates). Every file applies to the running device as it lands (live reconfiguration), with two boot-only exceptions the dialog names: network settings (bring-up is not re-runnable live, so the dialog offers the restart that applies them) and the web server's own `port` (binds at boot).
- `show hidden` — reveal dot-prefixed files/folders (e.g. `.config`); forwarded to `/api/dir` as its `hidden` filter.
- `filesystem` — read-only usage bar (used / total bytes, from the platform).
- `lastSaved` — read-only; how long ago config was persisted (read from the Filesystem engine).

Detail: [technical](moxygen/FileManagerModule.md)

<a id="i2c-scan"></a>

### I2C scan

A fixed System module (wired-by-code, always present) that probes the I²C bus on a button press and reports the addresses found — a hardware bring-up tool. The bus pins default to unused (−1), so a board without an I²C device claims no GPIO for it; a board with a bus sets its pins via the catalog, or you type them for an ad-hoc scan. Passive until the scan button is pressed.

<img src="../../assets/core/I2cScanModule.png" width="300" alt="I2C scan module controls">

- `sda` / `scl` — the bus GPIOs (default −1 = unused; a board with a fixed bus injects its own via the catalog, or type the pins for an ad-hoc scan — the classic Arduino-ESP32 pair is 21/22).
- `scan` — a button; press to probe the bus now.
- read-only — `result` (addresses found).

Detail: [technical](moxygen/I2cScanModule.md)

<a id="tasks"></a>

### Tasks

A read-only diagnostic that shows **what runs where** — the observability foundation for core-affinity / task-assignment work (you can't optimise which module runs on which core until you can see it). A fixed System module, wired-by-code. Inspired by MoonLight's task table; projectMM nests the MoonModules that run in each task beneath it, and the per-module cost comes from projectMM's own self-report (`tickTimeUs`/`classSize`/`dynamicBytes`) at zero extra cost. The raw FreeRTOS task view sits behind the platform boundary.

- read-only — `tasks` (a row per FreeRTOS task: `name`, `state`, `core`, `prio`, `stack` = min free stack ever seen; `cpu`% only in a build with `MM_TASK_CPU_STATS` — a `--task-cpu-stats` profiling build, off by default because the FreeRTOS run-time counter costs ~5% tick). Expand a row to see the MoonModules running in that task, each `Name · Nus · NB · Nheap` (`us` = average loop time, `class` bytes, `heap` bytes), plus a closing `∑ modules Xus / tick Yus · Zus outside modules` cross-check — the top-level module loop times should account for nearly all the tick, the small remainder being the blend/map/output that isn't a module. Today every module runs in the one render task, so that task's detail is the whole module list. Empty on desktop / a chip without the trace facility.
- read-only — `core0` / `core1` (the task currently executing on each core; empty on a single-core chip).

Detail: [technical](moxygen/TasksModule.md)

<a id="pins"></a>

### Pins

A read-only diagnostic that shows **which module owns each GPIO, for what role, and whether that pin is safe for it** — the device's pin ownership map, keyed by physical GPIO the way an OS Device Manager, a Tasmota template, or a GPIOViewer diagram is. A fixed System module, wired-by-code, always present. It walks the live module tree and collects every claimed pin — each GPIO control (a mic's `sckPin`/`wsPin`/`sdPin`, an Ethernet PHY's `ethMdcGpio`, a driver's `loopbackTxPin`) and each LED-driver `pins` lane CSV — so it needs no state of its own: unlike a central pin manager, each module owns its pins and this one only observes. A GPIO claimed by two controls is flagged red at the summary and lists both owners in the row detail — the read-only way to surface a conflict (a mic pin colliding with an LED lane, two lanes on one pin) without wedging the device: the claim still lands, the map just makes it loud. A **disabled** module's pins drop out of the map (switching a module off frees its GPIOs, on re-claims them) — the intent side of releasing resources on disable. Refreshes once a second, so a live pin change shows without a reboot.

- read-only — `pins` (a row per claimed GPIO: `gpio`, `owner` = the owning module, `role` = derived from the control name — `sckPin`→BCLK, `wsPin`→WS, `pins`→LED lane N, `ethMdcGpio`→MDC, …). A row is flagged with a colored edge when the claim is unsafe: **error** (red) for a claim on a reserved flash/PSRAM/USB pin or a double-claim, **warn** (yellow) for a driven role on a boot strap or input-only pin. The strap/reserved data comes from [gpio-usage.md](../../reference/gpio-usage.md) via the platform layer; PSRAM-conditional pins (classic-ESP32 16/17, S3 33-37) are flagged only when PSRAM is actually present at runtime, so a bare-WROOM board isn't falsely flagged. Each row also shows the pin's **live state** — `dir` (out/in/both/off, the pad's *actual* direction right now — shown as information, not auto-flagged, since a pin reading input/off is often legitimate: an idle I²C line, an unrun loopback pin, an external clock), `level` (HIGH/LOW, read straight off the pad — a driver's output must toggle when it renders, a mic clock must toggle when the mic runs), and `drive` (WEAK…STRONGEST). Expand a row to see every claim on that GPIO (`owner · role`) plus a `warning` line naming *why* it's flagged; a double-claim lists all co-owners. Unused pins (value −1) are skipped.

Detail: [technical](moxygen/PinsModule.md)

## MQTT — details

The topic prefix is `projectMM/<mac>` — a **stable** identifier (the last 6 hex of the device's MAC), fixed for the device's life. Renaming the device does **not** change its topics, so a hub's config never breaks on a rename (the WLED/Tasmota/Home-Assistant convention). It's derived, not a stored control.

**Topics** (for a device whose MAC ends `563cfe`): the device SUBSCRIBEs to the `set` topics and PUBLISHes the `get` topics on change (and on connect, so a controller never reads "No Response"). It also publishes its friendly `deviceName` on the retained `name` topic, so a hub can show the human name while the topics stay MAC-stable:

| direction | topic | payload |
|---|---|---|
| set → device | `projectMM/563cfe/on/set` | `true` / `false` |
| device → get | `projectMM/563cfe/on/get` | `true` / `false` |
| set → device | `projectMM/563cfe/brightness/set` | `0`–`100` |
| device → get | `projectMM/563cfe/brightness/get` | `0`–`100` |
| set → device | `projectMM/563cfe/hsv/set` | `h,s,v` (hue `0`–`359`, sat/val `0`–`100`) |
| device → get | `projectMM/563cfe/hsv/get` | `h,s,v` |
| device → get | `projectMM/563cfe/name` | the friendly `deviceName` (retained) |
| device → get | `projectMM/563cfe/update/state` | `{"installed_version":…,"latest_version":…,"release_url":…,"title":…}` (retained; HA update entity) |
| set → device | `projectMM/563cfe/update/set` | target version string (empty = install latest); triggers OTA against the matching GitHub release asset |

The HomeKit color wheel has no "palette" concept, so `hsv/set`'s hue+saturation pick the **nearest palette** (each built-in palette has a representative color; the closest one is selected) and the value drives brightness — the color wheel becomes a natural palette selector.

**Homebridge** — install [`homebridge-mqttthing`](https://github.com/arachnetech/homebridge-mqttthing) and add a `lightbulb` accessory. Use the device's own MAC suffix (read it from the `mqtt_status`/topics, or `mosquitto_sub -t 'projectMM/#'`) in place of `563cfe`:

```json
{
  "accessory": "mqttthing",
  "type": "lightbulb",
  "name": "projectMM",
  "url": "mqtt://<broker>:1883",
  "username": "<user>",
  "password": "<pass>",
  "topics": {
    "getOn": "projectMM/563cfe/on/get",
    "setOn": "projectMM/563cfe/on/set",
    "getBrightness": "projectMM/563cfe/brightness/get",
    "setBrightness": "projectMM/563cfe/brightness/set",
    "getHSV": "projectMM/563cfe/hsv/get",
    "setHSV": "projectMM/563cfe/hsv/set"
  },
  "onValue": "true",
  "offValue": "false"
}
```

Home Assistant adopts the device two ways, both zero-config:
- **MQTT auto-discovery** — with `haDiscovery` on (opt-in; off by default) and a broker set, the device announces itself on `homeassistant/light/projectMM_<mac6>/config` and HA auto-creates a wired entity with **on/off + brightness** (the config declares `brightness` only; color isn't in it, so the entity has no color control). Retained across reboots. Color/palette stays on the separate `hsv/set` topic above, not this entity. Off by default because the WLED `/json` shim already gives HA a richer light (color + palette + sensors) over mDNS with no broker — leaving both on lists the device twice; enable this only for broker-only / cross-subnet setups.
- **WLED integration** — HA's built-in WLED integration discovers the device over the WLED `/json` API projectMM already serves; on/off + brightness work with no broker.

Both can be on at once. Setup walkthrough (including exposing HA to Apple Home via HA's HomeKit Bridge, no Homebridge needed) in the [Home Assistant recipe](../../usecases/home-automation.md#adopt-in-home-assistant).

## File Manager — details

The panel is a lazy folder **tree** (each folder loads its children on first expand) plus an inline text editor. Dot-prefixed entries (the `.config` persistence dir) are hidden unless `show hidden` is on.

- Click a folder's row to select it and toggle its expansion (▸/▾); click a selected file to open the editor.
- The toolbar acts on the selected node: **＋ folder** creates a folder inside it, **＋ file** creates an empty file (click it to edit), **🗑 delete** removes the selected file, or a folder and everything inside it (press-twice to confirm), **⟳** refreshes.
- **Drag files from the desktop** onto a folder (or the tree) to upload them — the body streams straight to the file (any size, binary-safe; capped only by a sanity limit and the free space, which it reports if short); a per-file **⤓** streams it back to the desktop.
- The editor loads a file's text, pretty-prints JSON on open, and saves atomically; a binary file (contains a NUL) loads read-only (use ⤓ to fetch it intact). Upload and download both stream, so neither truncates.
- Create / delete are HTTP calls (`POST` / `DELETE /api/dir?path=`), not controls — the path rides the request, so nothing is stored on the device per op.

Last-modified dates (needs an NTP time source + LittleFS mtime), binary/large + folder upload, folder-as-zip download, and `.ml` syntax highlighting are backlogged ([backlog-core § File Manager follow-ups](../../backlog/backlog-core.md#file-manager-follow-ups)).
