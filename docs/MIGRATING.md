# Migrating

The log of **breaking changes** — what changed between versions, and the action to take.

projectMM ships **no migration code**: the persistence layer is robust by default (an absent key keeps the control's default, a stale value clamps to the new bounds, an unknown key is ignored), which absorbs almost all schema drift with zero migration-specific code. The rare change that a robust reader *cannot* absorb is **documented here instead of migrated** — see [ADR-0013](adr/0013-no-migration-code-robust-persistence-plus-documented-breaks.md) for the decision and its rationale.

**The File Manager's Backup (⤓) / Restore (⟲) carries config across these breaks.** [src/ui/migrate.js](https://github.com/MoonModules/projectMM/blob/main/src/ui/migrate.js) is the **authoritative, dated log of every machine-mappable break** (file, type, control, and value renames): Restore applies it in the browser and reports what did not carry over, so entries below describe only what a map cannot express, behavior changes, semantics to re-check, and erase-flash moves. It works even on a freshly erased device: join its `MM-XXXX` SoftAP, open `http://4.3.2.1`, restore there, and take the offered restart; the bundle carries the WiFi credentials, so the device comes back on your network. For a device still on old firmware (no Backup button yet), the [installer page](https://moonmodules.org/projectMM/install/) offers the same backup as a bookmarklet.

**Read this when upgrading a device that already holds persisted state.** Entries are newest first. Each says what changed and what to do; most need nothing at all, because the lost value re-populates on next use.

**MoonLive is exempt until it launches.** Nobody is running scripts on a device yet, so a break in the script language or its storage cannot strand anyone, and an entry here would describe an upgrade path no user can take. Its breaking changes are recorded in the commit and PR record instead. This exemption ends at the first release that ships MoonLive as a supported feature; from then it follows the same rule as everything else.

**Action legend** — how much work an entry costs you:

| Action | Meaning |
|---|---|
| *nothing* | Self-heals. The value re-populates on next use, or the default is correct. |
| *re-set a control* | One value resets to its default; set it again in the UI if you had changed it. |
| *re-add a module* | The module vanishes from the tree on boot; add it again and re-enter its controls. |
| *update a file* | An on-device file must be edited or replaced. |
| *erase flash* | A full flash erase is required (the heaviest — a full reconfigure follows). |

---

## Unreleased (`next-iteration`)

### Audio: `floor` is now the silence threshold in both level modes

**Action: re-set `floor` on a device whose microphone you had tuned.**
Affects any device running the Audio module with a local microphone or line-in.

`levels = automatic` is tuned with `floor` alone: how hard the learner levels, and how far it may
lift a band, are constants rather than controls, because both act on a per-band range the
conditioner has already normalized per rig, so one value serves every source.

`floor` is what changes meaning, and why re-setting it is worth a minute. It is now the **silence
threshold** in both modes: below it a band reads zero and the learner does not learn from it. That
is what stops a quiet room being amplified to full scale, but it also means a `floor` tuned under
the old behavior can now gate audible sound. Raise it until a silent room reads still, then stop;
there is no second knob to compensate with. `gain` remains manual-only and keeps its meaning.

Two behavior changes ride along and need no action. The spectrum now starts at 40 Hz rather than
~11 Hz, dropping a first band that could only ever hold mains hum, DC drift and rumble. And
AudioSpectrum's VU bar reads the raw level instead of the smoothed one, because it is the audio
test instrument and wants maximum response; every other effect keeps the calm smoothed VU.

### MoonBase serves the OTA routes under the application's names

**Action: nothing on most devices; a serial flash on a MoonBase device updated from a browser.**
Affects the 4 MB classic, `esp32-16mb` and the S3-Zero, the variants that carry MoonBase.

MoonBase served `/install`, `/install-url`, `/boot-app`, `/last-url` and `/cancel` while the
application served `/api/firmware/upload`, `/api/firmware/url` and `/api/firmware/moonbase`: two
names for one operation, across images that a single browser page talks to in turn during one
update. It now serves them under the application's names.

The break is between the two images on a device, not between a device and its config. A device
whose MoonBase predates this change still answers only the old names, so an updated application
handing over to it leaves the browser calling routes that image does not have. The way through is
the same as any MoonBase update: flash both images over serial once
([building.md](building.md#flashing-a-running-device-over-the-network)). A device flashed serially
from this version on is consistent and needs nothing.

### `soundReactive` is now `audioReactive`

**Action: re-set one control.** Affects Fish Tank, Flying Toasters, Pacman, Pong, Space Invaders,
Sprite Fountain and MovingHead, if you had turned the control on.

One name for one thing: the service is `AudioService`, the frame is `AudioFrame`, the effects are
audio-reactive. The control that made a sprite effect follow the music was the last place still
calling it sound, so it is renamed rather than left as the odd one out.

A restored config maps the old name to the new one and carries its value. On a device upgraded in
place the control returns to its default (off); switch it back on where you had it.

### AudioVolume is gone

**Action: pick another effect.** Affects any device with an AudioVolume effect on a layer.

It drew one bar from the audio level, which every audio-reactive effect does as a side effect of
what it actually draws. There is no successor to map it onto, so a restored config carrying an
`AudioVolumeEffect` node finds no such type and the layer comes up without it. `GEQ` is the nearest
thing if a literal meter is what you want.

### The Firmware card describes one image at a time

**Action: none.** Affects nothing a user has set: every control involved is read-only.

`firmwarePartition` is now `partition`, and `update_pct` is gone (an install's progress belongs in
the overlay the UI raises while it runs, not in a row that sits at zero for the life of a device
that is not mid-install). Where a device carries two images, a new `image` control selects whether
those rows describe the running app or MoonBase in the factory slot.

### Noise2D is gone; Noise renders it

**Action: re-set one control.** Affects any device with a Noise2D effect on a layer.

The two noise effects were one effect with two names: `Noise` is `Dim::D3` and draws the identical
field on a panel, so the 2D variant earned nothing. A restored config maps `Noise2DEffect` to
`NoiseEffect` and carries `scale` across.

What does not carry is `speed`. Noise2D took a 0..15 divisor of its own; Noise takes its rate from
`bpm` on the shared beat clock, so there is no value to map onto. Set `bpm` to taste after
restoring.

### Infrared is a list of learned rows, and the remote must be re-learned

**Action: re-learn the remote.** Affects any device with a configured infrared service.

`IrService` becomes `InfraredService`, rebuilt around rows: a row learns a code and points it at any
`Module.control`, where the old module carried five fixed actions (`code on/off`, `code brightness
up`, and so on) each bound to one predetermined behavior. The module itself carries over through
[migrate.js](https://github.com/MoonModules/projectMM/blob/main/src/ui/migrate.js)'s type map, so it
does not vanish from the tree, but the codes it held have no equivalent: a learned code used to be a
control's value, and is now a row. Press the remote's keys again against the rows you want.

Restoring a backup taken before the change reports the rename and flags the module for review rather
than silently dropping it. A device upgraded WITHOUT restoring a backup keeps its infrared module
and loses the codes.

### The desktop build keeps its files in `build/fs`, not `build`

**Action: move your data, or lose your settings.** Affects the DESKTOP build only, and only a
developer running it from a repository checkout; devices are unaffected.

A desktop install used the build directory itself as the device's filesystem, so the File Manager's
root listed CMake caches, object archives and every ESP32 variant's build folder alongside the four
directories a device actually has. It now roots at `build/fs`, so what the desktop shows is what a
board shows.

An existing checkout starts with an empty-looking device, because its `.config` is one level up.
Move what you want to keep:

```sh
mkdir -p build/fs
mv build/.config build/moonlive build/.hls build/fs/ 2>/dev/null
```

Nothing is deleted if you skip this: the old directories stay where they are, and the device simply
starts fresh. `MM_DATA_DIR` still overrides the location, and a packaged desktop install (which uses
the per-user data directory) is unchanged.

### projectMM no longer appears in WLED apps by default

Device discovery now announces on the multicast group `239.255.77.77` and, by default, **not** on
the broadcast address WLED apps and devices browse. A projectMM device therefore stops showing up
in them until you turn on `wledCompatible` in the Devices module.

projectMM devices still find each other either way: presence always goes to the group and every
device always joins it, so a fleet can mix the setting freely.

The reason for the default: a broadcast at discovery cadence makes every phone, printer and laptop
on the LAN take an interrupt and parse a packet none of them want. Multicast reaches only the
devices that joined the group. See
[multicast and IGMP snooping](architecture.md#multicast-and-igmp-snooping) for when that saving is
real (a switch that snoops) and when it is not.

### A light preset's Dimmer channel is now driven

A preset that declares a `Dimmer` role previously left that channel at 0, because nothing ever
wrote it: `Correction` resolved only the color roles. A fixture on such a preset therefore emitted
nothing at all, whatever its color channels said. The shipped `IRGB` preset ("CH1 master
intensity") could never light a fixture.

The dimmer is now held open (255) every frame, with per-light brightness staying in the color
values as before. **If you drive a fixture on `IRGB` or another dimmer-carrying preset, it will
light up where it previously stayed dark.** Nothing to change; the previous behavior was a defect.

Routing brightness to the dimmer channel rather than holding it open is the better model and is
[backlogged](backlog/backlog-light.md), so this value will change again.


### esp32-16mb moves to the MoonBase partition table (2026-08-28)

**Action: erase flash** (USB re-flash). Back up first (File Manager, or the installer's
bookmarklet on older firmware); restore after the install brings WiFi, config and scripts back.

`esp32-16mb` replaces its dual-OTA layout with
[MoonBase](architecture.md#moonbase-the-second-boot-image), the same trade the 4 MB variants
made in the entry below, taken here by choice rather than necessity: the second app slot was
idle except during an update, so the filesystem grows 7168 to 11264 KB and the device gains
MoonBase's stronger recovery story (a power cut mid-install boots MoonBase and the user retries
over the network). One app slot remains, at its full 4096 KB.

Every partition moves, so the existing filesystem volume is not where the new table looks:
without a backup, WiFi credentials, module config and scripts all re-enter through provisioning.
A partition table only changes over USB, so an OTA update leaves a device on the old layout.

### 4 MB boards move to the MoonBase partition table (2026-08-26)

**Action: erase flash** (USB re-flash). Back up first (File Manager ⤓, or the installer's
bookmarklet on older firmware); restore after the install brings WiFi, config and scripts back.

The 4 MB variants (`esp32`, `esp32-wrover`, `esp32-eth`) replace the dual-OTA layout with
[MoonBase](architecture.md#moonbase-the-second-boot-image): the app slot grows
1856 → 2496 KB and the filesystem 256 → 548 KB, but the filesystem moves (0x3B0000 → 0x360000),
so the existing volume is not where the new table looks; without a backup, WiFi credentials,
module config and scripts all re-enter through provisioning. A partition table only changes over USB: a device
still on the old table keeps OTA-updating *within* that table for as long as the app fits its
1856 KB slot; the web installer is the migration path. 8/16 MB boards are unaffected.

### PreviewDriver's `fps` becomes `targetFps`, and now trades resolution (2026-08-25)

The control is renamed and its meaning changed, so the rename is the point rather than cosmetic.

**Before:** `fps` was a ceiling. The driver never exceeded it, but a link that could not sustain the rate simply delivered fewer frames and the control did nothing about it.

**Now:** `targetFps` is the rate you *want*. The driver still never exceeds it, and when the link cannot keep up it **trades preview resolution** to get closer, lower it for full detail at a slower rate, raise it for a smoother but coarser preview. That makes the slider the place where you choose between detail and smoothness, which is what users were reaching for.

**Action: none required.** The preview is a view, not output. A device that had a non-default `fps` saved falls back to the default 24 on first boot with this firmware, because the persisted key changed; set `targetFps` if you had tuned it. Mixed versions degrade soft: an old UI against new firmware sends no detail request and gets full detail (capped by memory); a new UI against old firmware sends an uplink message the device ignores.

### A module declares every control with `addControl` (2026-08-24)

`addUint8`, `addUint16`, `addInt16`, `addInt32` and `addBool` are replaced by one overloaded
`addControl(name, variable, min, max)`. The widget follows the variable's own type, which the
compiler already knows, so the name no longer repeats a width the declaration states:

```cpp
controls_.addUint8("speed", speed_, 1, 255);     // before
controls_.addControl("speed", speed_, 1, 255);   // after
```

This is the same call a MoonLive script makes, which is the point: someone who has written a
script can read a compiled module, and someone who has read a module can write a script.

The **widget-specific** adders keep their names — `addPin`, `addSelect`, `addPalette`, `addText`,
`addTextArea`, `addFilePath`, `addPassword`, `addIPv4`, `addReadOnly`, `addReadOnlyInt`,
`addProgress`, `addList`, `addButton`. Those name a widget rather than a width, and the intent is
not recoverable from the C++ type: `uint8_t` backs a slider, a dropdown *and* a palette picker, and
an `int8_t` silently becoming a Pin would register as a claimed GPIO in the pin map. `addControl`
on an `int8_t` is deliberately deleted, with a diagnostic naming the two real options.

**Action: *nothing* for a device.** No control name, type, range, wire format or persisted value
changes — a renamed call produces a byte-identical descriptor, which is why nothing on the device
can notice.

**Action for a third-party module: *recompile*.** Rename the five calls to `addControl`; the
arguments are unchanged. A missed one is a compile error, never a silent behaviour change: the
overloads bind by exact reference type, so a call that compiles produces the widget it always did.


### Desktop settings move to a per-user directory (2026-08-23)

The desktop build wrote its configuration to `build/.config`, resolved against whatever directory the process happened to start in. That is a source-checkout layout, and it shipped: a downloaded binary either could not write there at all, failing every save and logging one line per save, or it wrote settings that belonged to that *folder* rather than to the user, so moving the executable lost them.

Settings now live with the user: `%LOCALAPPDATA%\projectMM` on Windows, `~/Library/Application Support/projectMM` on macOS, and `$XDG_DATA_HOME/projectMM` on Linux, falling back to `~/.local/share/projectMM` when that is unset. `MM_DATA_DIR` overrides it. **A source checkout is unchanged** and still uses `build/.config`, so a development tree and every gate script behave exactly as before.

**Action: *nothing*, unless your settings actually persisted before.** The old behavior had two modes, and only one of them leaves anything to move:

- **Saves were failing.** The log showed `write failed for /.config/...` on every change and nothing survived a restart. Nothing to carry across.
- **Saves were succeeding, per folder.** They are in a `build/.config` folder beside wherever you launched from: the folder you unzipped into on Windows and Linux, and `~/build/.config` on macOS, because the `.app` launcher starts in your home directory. **Action: *move a folder*.** Move the `.config` directory itself into the new per-user directory, so it lands as `<data directory>/.config` rather than spilling its files into the root. Or leave it and reconfigure from scratch.

ESP32 is unaffected: LittleFS mounts at a fixed partition and never used this path.

### The `Layers` container is renamed to `Effects` (2026-08-08)

The three top-level light containers are now **Layouts, Effects, Drivers** — L.E.D. The old name sat one character from its own child (`Layers` holding `Layer`s) and read as a near-twin of `Layouts`, which is the pair a newcomer actually has to tell apart. The tree is unchanged in shape: `Effects` → `Layer`s → effects and modifiers.

**Action: *re-add a module* and *re-save presets*.**

The type name is the persisted filename and the preset capture key, so two things do not survive the update:

| What | Why | What to do |
|---|---|---|
| The saved light tree | The device looks for `/.config/Effects.json` and the old file is `Layers.json`, so the light tree boots empty | Re-add your Layer, effect and modifiers, then let it save |
| Presets that capture the look | A preset file records `"captures": "Layers"`, a name no module now answers to | Re-save each preset once the tree is rebuilt |

A preset also records the ROLE it covers, and that role is now named after the container rather than after a module inside it: `"layer"` becomes `"effects"`. A preset carrying the old role still loads, but shows no tint on its pad until it is re-saved — the UI has no `layer` role to colour it by.

The child `Layer` keeps its name, as does everything under it.


### The `peripheral` options are renamed to name the peripheral, not the bus protocol (2026-07-30)

The `peripheral` dropdown no longer says `i80` / `MoonI80`. "i80" is the Intel 8080 bus shape `esp_lcd` speaks — it is not a peripheral any ESP32 datasheet lists, and it matched nothing a user could look up: on the classic ESP32 that backend **is the I2S peripheral**, on the S3/P4/S31 it is the **LCD** peripheral. The new labels name the silicon block plus who drives it, which is the actual choice being made.

| Old | New (classic ESP32) | New (S3 / P4 / S31) |
|---|---|---|
| `i80` | `I2S-IDF` | `LCD-IDF` |
| `MoonI80` | — (not available) | `LCD-MM` |
| `Parlio` | — | `Parlio` (unchanged — it *is* the peripheral's name) |

`-IDF` = driven through ESP-IDF's `esp_lcd`; `-MM` = driven by our own GDMA layer below it, which is what buys the streaming ring and the 74HCT595 pin expander.

**Action: re-set the `peripheral` control** — but only on a device that already holds a persisted parallel driver AND had a non-default peripheral selected. The stored string no longer matches any option, so the loader falls back to the board's default backend; if that was already your choice, nothing changes. The web installer's board catalog ships the new names, so a fresh install or catalog re-inject is correct without action.

### The three parallel LED drivers merge into one `ParallelLedDriver` with a `peripheral` selector (2026-07-23)

`MultiPinLedDriver`, `MoonLedDriver`, and `ParlioLedDriver` are now one registered module, **`ParallelLedDriver`**, whose `peripheral` control picks which DMA peripheral drives the parallel WS2812 bus. They were always the same driver with a different bus backend; the merge makes that one card with a dropdown, offering only the peripherals the chip supports.

| Old registered type | New |
|---|---|
| `MultiPinLedDriver` | `ParallelLedDriver` + `peripheral` = `i80` (esp_lcd: LCD_CAM on S3/P4, I2S on classic) — renamed again below |
| `MoonLedDriver` | `ParallelLedDriver` + `peripheral` = `MoonI80` (own-GDMA below esp_lcd, LCD_CAM) — renamed again below |
| `ParlioLedDriver` | `ParallelLedDriver` + `peripheral` = `Parlio` (P4) |

**Action: re-add the driver.** A persisted module whose type is one of the three old names no longer resolves (the type isn't registered), so the robust loader drops it on boot — the driver, and its pins/settings, vanish from the tree. Add a **Parallel LED** driver again, choose the `peripheral` your board uses (the same backend the old type named — see the table), and re-enter its `pins` / `ledsPerPin` plus whatever the chosen peripheral needs: `i80` has `clockPin`/`dcPin`, `MoonI80` has `clockPin` + the ring/expander controls, `Parlio` has no clock or DC pins at all. The web installer's board catalog already names the new type, so a fresh install or a catalog re-inject wires it correctly; only a device carrying an OLD persisted tree needs the manual re-add.

### The per-driver `preset` control is renamed to `lightPreset` (2026-07-23)

**Action: nothing** on-device (the saved value survives, see the `lightPreset` [persistence contract](moonmodules/light/drivers.md#led-driver-details)). Only an external script or automation that POSTs the control by name (`/api/control` with `"control":"preset"`) must switch to `lightPreset`.

### `AudioService`: the `sync` control becomes `mode` + `send audio`, and `simulate` is renumbered (2026-07-22)

The audio module's identity is now a single `mode` control (Local audio / Receive network / Simulate), each showing only its own detail controls, replacing the separate `sync` (off / send / receive) toggle. Broadcasting the locally-analyzed frame moved to a `send audio` switch, meaningful only in Local mode. `simulate` was also renumbered, from a five-option list to two.

| Old | New |
|---|---|
| control `sync` (Select: `off`/`send`/`receive`) | `mode` (Select: `local audio`/`receive network`/`simulate`) + `send audio` (a switch, Local mode only) |
| control `simulate` (Select, 5 options incl. a mic-fill-on-silence mode) | `simulate` (Select: 2 options) — used only when `mode` is Simulate |

**Action: re-set `mode` (and `send audio`) if you had `sync` on `send` or `receive`; re-set `simulate` if you had chosen a non-default option.**

`sync` and the old `simulate` value read as absent → ignored, so `mode` takes its default (**Local audio**) and `send audio` its default (**off**). A device that was on `sync=receive` therefore comes up as Local audio — set `mode` to Receive network again. One that broadcast (`sync=send`) comes up not broadcasting — turn `send audio` on. The five-option `simulate` collapsed to two, so a device on one of the dropped options (e.g. the mic-fill-on-silence mode, a removed capability) takes the new default; re-pick if needed. Receive network and every sync control exist only on network-capable targets.

### `MoonLedDriver`: `forceRing` → `useRing`, and the ring's geometry is now settable (2026-07-17)

The pin-expander path selector was a three-option Select (`auto` / `ring` / `wholeFrame`) named for a *diagnostic override*. The auto-router is gone — at the size the expander exists for (48 strands × 256 lights) a whole frame never fits internal DMA RAM, so "auto" had exactly one right answer while presenting itself as a choice, and its silent fallback hid which path was actually running. What remains is the honest question, as a switch:

| Old | New |
|---|---|
| control `forceRing` (Select: `auto`/`ring`/`wholeFrame`) | `useRing` (a switch: on = ring, off = whole frame) |
| — | `ringRows` (new: lights per DMA buffer, 1..64) |
| — | `ringBufs` (new: buffers the DMA circulates, 2..32) |

**Action: re-set `useRing` if you had `forceRing` on `wholeFrame`.**

`forceRing` reads as absent → ignored, and `useRing` takes its default (**on**, the ring). A device that had explicitly selected whole-frame therefore comes up on the ring; flip `useRing` off to get it back. `ringRows`/`ringBufs` default to 16 and 12 — the geometry the driver effectively ran. (It shipped with a pool of 16, but 16 buffers never fit the S3's internal DMA heap, so the ring build failed its own fit check and the driver quietly fell back to whole-frame; 12 is what actually held. A config on the old defaults may therefore start *ringing* where it used to fall back.) They exist so the RAM / encode-overhead / interrupt-rate / lap-time trade-off can be swept on a live board rather than fixed at compile time.

### LED driver + control rename — a human-readable UI (2026-07-16)

The LED driver module types and several controls were renamed so the UI reads in plain language rather than peripheral jargon (the UI shows a control's name verbatim, so the name *is* the label).

| Old | New |
|---|---|
| module type `I80LedDriver` | `MultiPinLedDriver` |
| module type `MoonI80LedDriver` | `MoonLedDriver` |
| control `shiftRegister` | `pinExpander` |
| control `asyncTransmit` | `doubleBuffer` |
| read-only `wireUs` | `frameTime` |
| read-only `stall` (Drivers) | `renderWait` |

**Action: re-add the module, then re-set `pinExpander` / `doubleBuffer` if you had changed them.**

A device whose persisted config names the old module type loads a module type that no longer exists — the unknown type is ignored, so **the driver is absent from the tree on boot**. Re-add a **Parallel LED** driver (the single type the two later merged into — see the 2026-07-23 entry above for the `peripheral` value that matches the old `I80LedDriver` / `MoonI80LedDriver`) and re-enter its controls. Within a re-added driver, the two renamed *settable* controls (`pinExpander`, `doubleBuffer`) read as absent → they take their defaults (`pinExpander` off, `doubleBuffer` on); set them again if your board needs otherwise. `frameTime` and `renderWait` are read-only KPIs — nothing to restore.

This rename left `RmtLedDriver` untouched, and `ParlioLedDriver` untouched *at the time*; the later 2026-07-23 entry above then merges `ParlioLedDriver` into `ParallelLedDriver` along with the other two. The `pins` / `ledsPerPin` / `clockPin` / `latchPin` / `loopback*` controls are unchanged by this rename.

---

## Earlier

These pre-date this log and were recorded in ADR-0013's Consequences list. A device that persisted state on an older build and loads a newer one loses only the noted value, which re-populates on next use.

### UI last-selected module (`mm.selectedModule` → `mm_selected`)

The browser localStorage key for the UI's last-selected module.

**Action: nothing.** Lost: the remembered selection resets to the first module.

### Device-list `color` → `color` (US-spelling rename)

The DevicesModule persisted-list key for a Hue bridge's color-capable light count (`DevicesModule::restoreList()`). A device list persisted under the old key reads the count as absent → 0.

**Action: nothing.** The cached bridge count resets to 0 until the bridge is re-heard live and re-populates it.
