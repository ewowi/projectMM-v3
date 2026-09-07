# Plan: MoonBase, a second boot image for 4 MB boards

## Context

The 4 MB ESP32 boards have run out of flash. CI caught it on 2026-08-26: the `esp32-wrover`
build is 1,839,776 bytes against an 1792 KB slot, and the hotfix that grew both OTA slots to
1856 KB left it at 3% free. The cause is structural: a dual-OTA layout spends half the chip on a
second copy of the firmware, so every kilobyte the app gains costs two.

**MoonBase** replaces that second copy with something smaller and more useful: a tiny, rarely
changing image in the `factory` partition that owns the device when the application is not running
or cannot be trusted. Its first job is installing firmware into the one large app slot (a device
cannot rewrite the partition it is executing from). It is named for the family it joins, alongside
MoonDeck, MoonLight and MoonLive, and it is deliberately not called "recovery": updating,
re-provisioning WiFi, factory reset and diagnostics are all maintenance, not repair.

Outcome for the 4 MB boards: the app partition grows from 1856 KB to 2496 KB (+34%), the
filesystem from 256 KB to 548 KB, and OTA keeps working (through MoonBase).

## The measurements this plan rests on

All from clean builds whose exit status was checked (two earlier figures in this plan's history
were wrong: one reported a stale binary, one was measured with the URL installer stubbed out).

Getting from ESP-IDF's defaults to a shippable size is mostly configuration, not code:

| Configuration | Size |
|---|---:|
| Bare ESP-IDF hello-world | 139 KB |
| WiFi + HTTP + OTA, IDF defaults (-O2) | 881 KB |
| + `-Os` | 812 KB |
| + no logs, no error strings, no console | 695 KB |
| + newlib-nano, no IPv6, no WPA3/enterprise | 588 KB |
| *MycilaSafeBoot esp32dev, for reference* | *640 KB* |

And the finished MoonBase, built against its own table:

| Build | Size |
|---|---:|
| Upload only | **576 KB** |
| Upload + install-from-URL (HTTPS) | **742 KB** |

**Install-from-URL costs 166 KB**, all of it TLS and the HTTPS OTA client. `-flto` was tried and
saved nothing (IDF appears to ignore it for the app image), so the cheap levers are spent. SoftAP
(~35 KB) is kept: without it a board whose stored credentials went stale is only recoverable over
USB, which is the situation MoonBase exists to avoid.

## The partition table

`esp32/partitions/esp32dev_moonbase.csv`. Fixed overhead (bootloader, table, nvs, otadata) is
92 KB; the rest fills the chip exactly, with every app offset 64 KB aligned:

| Region | Type/SubType | Offset | Size | Was |
|---|---|---|---:|---:|
| nvs | data/nvs | 0x9000 | 20 KB | unchanged |
| otadata | data/ota | 0xE000 | 8 KB | unchanged |
| **moonbase** | app/factory | 0x10000 | **896 KB** | new |
| **app** | app/ota_0 | 0xF0000 | **2496 KB** | 1856 KB |
| **littlefs** | data/littlefs | 0x360000 | **548 KB** | 256 KB |
| coredump | data/coredump | 0x3E9000 | 64 KB | unchanged |

896 KB holds the measured 742 KB image with 154 KB spare. HTTPS is 166 KB of that image and is
kept deliberately: install-from-URL is what lets a device fetch its own release instead of having
the file pushed from whatever machine is in front of it, and the releases live on GitHub, which is
HTTPS-only. Serving firmware over plain HTTP instead would mean the device executes whatever an
attacker on the path substituted; signing the image would cost comparable space plus real work.

The headroom is sized for a new COMPONENT rather than for features: factory reset, re-provisioning,
config backup and diagnostics are a few KB each, while one component can cost more than all of them
together. An undersized factory partition cannot be regrown without a second full-erase migration
of every device in the field, so it is budgeted long once.

The filesystem partition is `littlefs` in both name and subtype (0x83, which ESP-IDF v6.1 and the
joltwallet driver both define). Older tables call the same volume `spiffs` with subtype 0x82, a
legacy misnomer since the contents have always been LittleFS. `platform_esp32_fs.cpp` now searches
subtype littlefs then spiffs, so a device that keeps an older table across an OTA still finds its
config; the 8/16 MB tables migrate in a later cycle, once every device carries that fallback.

## MoonBase itself

A standalone ESP-IDF project at **`moonbase/`** (a root folder, matching `moondeck/` and
`moonlive/`), `project(projectMM-moonbase)`, emitting
`projectMM-moonbase.bin`. The distinct name matters: `projectMM.bin` is matched by basename in
`release.yml:214`, `flash_esp32.py:118`, `moondeck/run/preview_installer.py:203` and
`generate_manifest.py:58`, which skips unknown basenames with only a warning.

**It shares no sources with the application.** The earlier attempt reused `platform_esp32.cpp` and
measured 788 KB with an empty `app_main`, because that file drags RMT, I2S, PSRAM and JIT support
plus their include surface. MoonBase is written against ESP-IDF directly: a few hundred lines, its
own `sdkconfig.defaults` carrying the size flags above, and no dependency on `src/`. That
duplication is the deliberate trade for an image that must stay small and, once working, hardly
change.

What it does, in order: bring up the network (stored WiFi
credentials, else its own AP at **4.3.2.1** matching `NetworkModule.h:943`; Ethernet is a
follow-up, see the backlog), then serve a single
page offering the maintenance actions, then reboot back into the app.

Version 1 ships exactly one action: **install firmware**, both by upload and by URL (the URL form
is what makes an unattended update possible, and is why HTTPS is in the budget). Credentials are
read from `/.config/NetworkModule.json` with a bounded key scan rather than a JSON parser.

Deliberately **not** in version 1, but the reason the name is broad: factory reset, WiFi
re-provisioning, config backup and restore, firmware downgrade, hardware diagnostics, and a
boot-with-config-disabled escape for a config that crashes the app. Each solves something only a
separate image can solve. Each also costs bytes, so each needs to earn its place.

## The mechanism

Verified in `~/esp/esp-idf/components/app_update/esp_ota_ops.c`:

- `esp_ota_get_next_update_partition` iterates only OTA subtypes and falls back to the first OTA
  slot found. From `factory` it returns `ota_0` (correct). **From `ota_0` it returns `ota_0`
  itself**, the running partition.
- `esp_ota_begin` refuses that case with `ESP_ERR_OTA_PARTITION_CONFLICT` (`esp_ota_ops.c:173`),
  so a direct upload fails safely rather than erasing the running app.
- `esp_ota_set_boot_partition` on a factory partition **erases otadata** rather than writing a
  sequence number, which is what makes the power-fail story work.

Already implemented on this branch (steps 1 and 2 below): the platform guards and the queries
`otaHasMoonBase()` / `otaBootMoonBase()` / `otaRunningMoonBase()`.

`HttpServerModule::handleFirmwareUpload` gains one branch: when a MoonBase partition exists and we
are not already running from it, reply 202 `{"moonbase":true}` and reboot into MoonBase. `app.js`
keeps the chosen file in memory, starts a countdown BEFORE the device reboots so there is no dead
gap, polls for actual reachability rather than trusting the clock, and re-POSTs automatically: one
click, one progress experience. If MoonBase fell back to its AP the device is no longer at the
polled address, so that case says so and names 4.3.2.1.

## Failure semantics

A failed install deliberately leaves the device in MoonBase, even when the old application is
still intact in the app slot. Auto-reverting was considered and rejected by the PO: a device that
silently comes back running the old firmware looks like a successful update that changed nothing,
which is confusing. Ending in MoonBase makes the failure visible (the update overlay reports the error, and
MoonBase's page shows the last install status on load) and leaves every option open: retry, try a
different image, or walk away and fix the network first.


At every instant, otadata is either blank (boots MoonBase) or points at an `ota_0` image that
`esp_ota_end` already validated. A power cut mid-write leaves blank otadata, so the board comes up
in MoonBase and the user retries over the network. This is a **stronger** power-fail story than
today's 4 MB dual-OTA layout.

Bootloader rollback stays disabled: it needs a second OTA slot to roll back to, and MoonBase is
the recovery path.

## Steps

1. **Platform guards** (done, uncommitted): reject an image larger than the target partition;
   reject a target equal to the running partition; add the three queries. Inert on today's tables,
   and independently valuable, since an oversized image currently fails mid-write with no check.
2. **Partition-table validity test** (done, uncommitted): `ctest` over `esp32/partitions/*.csv`
   for overlaps, bounds, 64 KB app alignment, and the dual-OTA-or-MoonBase shape rule. Verified by
   deliberate faults (overlap, misalignment, mixed shape each fail).
3. **MoonBase v1** (done): `moonbase/` with its size-tuned sdkconfig, the WiFi + SoftAP cascade,
   one served page, install-by-upload (raw body, no multipart parsing) and install-by-URL over
   HTTPS. Measured 742 KB. Ethernet is a follow-up: the app's `ethInit()` needs per-board pin
   configuration, and only the eth-only 4 MB variants want it.
4. **The partition table** (done): `esp32dev_moonbase.csv` plus the
   `sdkconfig.defaults.moonbase-4mb` fragment, pinned by the step-2 test.
5. **Bench MoonBase standalone** (done for WiFi): hand-flashed at 0x10000; the AP at 4.3.2.1 and
   its page verified by the PO. Stored-credential WiFi and a full install still open, folded into
   the step-6 bench below. Side finding: opening the serial port can bounce a classic ESP32 into
   ROM download mode (DTR/RTS auto-reset), which mimics a dead board; verification is by network,
   not by serial.
6. **Wire the 4 MB variants** (done): the four variants carry a `moonbase` flag in `FIRMWARES`;
   `build_esp32.py` appends the fragment (last, so it wins) and builds `moonbase/` into
   `build/moonbase-<chip>/`; `stale_feature_cache` now also wipes a build dir whose fragment list
   or generated partition table no longer matches (IDF never regenerates sdkconfig on its own).
   `flash_esp32.py` writes the corrected layout in one pass (app at ota_0, MoonBase at factory,
   a slot-0 otadata so the fresh flash boots the app with MoonBase as fallback).
   `check_firmwares.py` verified the flag stays out of `firmwares.json`. Olimex erased and
   flashed through this exact path; boot from ota_0 bench-verified.
7. **The switch route and UI** (done). Bench record: the one-click FILE install ran
   PO-verified through the overlay; the unattended URL cycle was verified at the mechanism
   level by curl (staged-NVS handoff, plain-HTTP for LAN sources, a 3-attempt retry absorbing
   the connect race right after GOT_IP), and the Reviewer then caught that the overlay itself
   could never see that path succeed (MoonBase installs before it serves, so success is silence
   then the new app), which is fixed; the overlay URL flow re-verifies via the moonbase-test
   release. The MoonBase button on the Firmware card and MoonBase's "Boot the app" are the two
   explicit ways across. The moonbase-test release then caught two GitHub-only failures the LAN
   test could not see: the TLS handshake overflowed the 3.5 KB main-task stack (now 12 KB), and
   GitHub's signed redirect overflowed the HTTP client's 512-byte header buffer (now 4 KB, the
   app's own OTA values); with both fixed, a GitHub HTTPS install completes in under 40 s.
   The unattended install then moved onto its own task so MoonBase serves while downloading:
   GET /moonbase reports "preparing the install" then "downloading: N of M bytes" live, the
   overlay renders that as the same progress bar the file path shows, and a second install (or
   Boot-the-app mid-write) gets a 409. PO-verified through the picker against the test release.
   The install then sped up 3x (25 to ~86 KB/s streamed; the whole URL install ~30 s): the rate
   was flash-bound, not network-bound: identical over TLS and plain HTTP, fixed by one bulk
   erase up front instead of per-sector erases inlined with the writes, plus 32 KB receive
   chunks; WiFi power save is also off in MoonBase (it throttled RTT 20x for no benefit).
   The power-cut procedure then ran (PO): the overlay reports the silence, and once MoonBase
   is back it re-submits the install from the payload the browser still holds; the cycle
   completes with no clicks. Ethernet shipped after that (classic RMII): MoonBase reads the
   eth wiring from the same config file as the credentials, runs ONE interface at a time in the
   app's own preference order (so the browser keeps the address the app had, PO decision), and
   an install over eth streams at the same flash-bound rate as WiFi. Bench note from that work: after the table migration the
   deviceModel catalog push had never been re-applied (ethType stood at 0), and applying
   ethType live did not bring eth up where the boot init did, an app-side observation worth
   its own look.
8. **CI and installer** (done): `build_esp32.py` owns the shared layout helpers
   (moonbase_table_csv / partition_offsets / otadata_slot0_bytes / moonbase_flash_files), the
   one place that corrects IDF's flasher_args, consumed by the serial flash, the manifests, the
   release preview and the QEMU image (its merged image verified at every offset). The slot-0
   otadata blob is byte-identical to otatool's own output (bench readback). release.yml stages
   shared-moonbase-<chip>.bin + shared-ota-data-slot0.bin; install-picker rejects both
   (pinned by a JS test); check_esp32_built also gates the MoonBase image's freshness. A
   temporary `moonbase-test-release.yml` workflow (push-triggered on this branch, since GitHub
   only registers a dispatchable workflow from the default branch; esp32 only) published a
   `moonbase-test` prerelease from this branch so the picker's URL install could be tested
   against real GitHub assets; it was deleted again before the merge, so it never reaches main.
   The `moonbase-test` release and tag on GitHub are deleted after the merge
   (`gh release delete moonbase-test --cleanup-tag`).
9. **Migration and docs** (done): architecture.md § MoonBase is the concept's one home;
   README feature bullet credits Tasmota's safeboot and MycilaSafeBoot; building.md notes the
   one-pass 4 MB flash; MIGRATING.md carries the erase-flash entry; the FirmwareUpdate catalog
   card documents the moonbase control; the resolved 4 MB flash-budget investigation is deleted
   from the backlog. The planned update-badge message for legacy-table devices was not built:
   OTA within the old table keeps working while the app fits its 1856 KB slot, so MIGRATING.md
   carries the migration story instead.

## Verification

- `cmake --build build` and `ctest` at every step, plus scenarios and the spec check.
- Host tests shipped: the partition-table case (unit_PartitionTables, verified by deliberate
  faults); the credentials-in-prefix contract (unit_MoonBaseContract pins ssid/password inside
  MoonBase's 1024-byte read of NetworkModule.json); the install-picker asset parse with MoonBase
  assets present (installer-firmware-merge). Planned but not built, with the reason: a pure-
  function credential-scraper test (the scraper lives in the MoonBase image, not in src/, and
  the contract test pins the cross-image half); the image-too-large rule (exercised on the bench
  through the platform guard); a synthetic-flasher_args manifest test (the manifest was verified
  against the real build's flasher_args instead).
- **Bench** (a rigorous change under CLAUDE.md: partition and boot changes can brick a board, so it
  gets a heads-up and a go-ahead before the first flash): MoonBase reachable on Ethernet and WiFi;
  AP fallback with bad credentials; a full update through the UI; a direct upload to the running
  app returning 202 and never starting an erase.
- **The power-cut procedure**: flash the layout, note the config contents, start an install, and
  physically cut power at ~50% (not `esp_restart()`). Expected: the board boots MoonBase, the
  network returns, a retry completes, and the config survives. Repeat at ~10% and ~95%.
