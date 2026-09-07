# Plan: Raw-L2 interface becomes a dropdown of detected NICs (Discord request)

## Context

Delphium (Windows panel-card tester) hit the classic trap: Windows NIC indices differ from
Npcap's, and the free-text `interface` control on PanelCardDriver means typing exact adapter
names. Request: a dropdown of DETECTED interfaces with friendly names. The PO approved
("Excellent idea!"). Key requirement drawn from his report: the choice must be stable by NAME,
never by index, across reboots and Npcap (re)installs.

## Facts found (reuse, don't rebuild)

- PanelCardDriver's `interface` is `char interface[]` + `addText`, consumed in `prepare()` via
  `platform::ethBindRawInterface(name)` (PanelCardDriver.h:201); blank = capture mode; bind
  failure is a Warning with the name echoed.
- The AudioService `device` Select is the dynamic-enumeration precedent (AudioService.h:210):
  options re-enumerated on every `defineControls()` rebuild via a platform call returning
  `(const char* const**, count)`; entry 0 is the order-stable default.
- Windows already resolves `pcap_findalldevs` (platform_desktop.cpp:889, `pcapFindAllDevs_`),
  which yields device name + friendly description. POSIX enumeration = `getifaddrs`.
- Select persistence writes the INDEX (Control.cpp:122), but the APPLY path accepts the option
  LABEL and matches it by string (Control.cpp:366, "the label is stable" rationale). Index
  persistence is exactly the instability Delphium reported.

## Design

**New per-control flag: persist a Select by LABEL.** `Control` gains `persistLabel` +
`ControlsList::setPersistLabel(i)` (the setNumberField/setReadOnly idiom, Control.h:602-625);
`writeControlValue`'s Select branch emits the option string when set. The apply path already
handles labels, so old index-persisted configs keep loading (robust both ways, ADR-0013). This
also becomes available to the audio `device` Select later, same instability class.

**New platform seam `rawInterfaces`** (platform.h, beside the raw-Eth block at :440):
`size_t rawInterfaces(const char* const** optionsOut)` returning a cached option list rebuilt
per call. Entry 0 is `"none (capture only)"`. Windows: `pcap_findalldevs`, each entry shown as
its friendly description with the pcap device name kept in a parallel array
(`rawInterfaceName(i)`) since `ethBindRawInterface` needs the real name; POSIX: `getifaddrs`
names (name = display name, no parallel array needed but keep the accessor uniform). ESP32:
declared, unused (the control only builds where `hasRawEth`-equivalent is true; check the
existing gate PanelCardDriver uses and mirror it).

**PanelCardDriver**: `interface` becomes a Select (`interfaceSel_` uint8) over
`platform::rawInterfaces()`, `setPersistLabel`, rebuilt every `defineControls()` so a
hot-plugged NIC appears on the next schema rebuild; `prepare()` binds
`rawInterfaceName(interfaceSel_)` (index 0 → nullptr = capture mode, today's blank semantics).
`affectsPrepare("interface")` unchanged. Bind-failure status text unchanged (still echoes the
name).

**Migration**: the old TEXT value is a raw interface name; the Select apply path label-matches
it, so an existing config whose NIC is present picks itself up unchanged. A vanished NIC
degrades to "none (capture only)" with the existing Warning status. No migrate.js entry needed
(same key, values are the same strings); the MoonLight-era MIGRATING.md gets nothing since the
loader absorbs it (mention in the card only if behavior questions arise).

## Files

- src/core/Control.h / Control.cpp — `persistLabel` flag + `setPersistLabel`; Select write path.
- src/platform/platform.h + platform_desktop.cpp — `rawInterfaces`/`rawInterfaceName` seam
  (pcap on Windows, getifaddrs on POSIX) + a test seam (`setTestRawInterfaces(list)`) so the
  control is pinnable without real NICs, mirroring the audio/NDI test-seam convention.
- src/light/drivers/PanelCardDriver.h — the Select.
- docs/moonmodules/light/drivers.md — the panel-card card's `interface` line.

## Tests

- unit (Control): a persist-label Select round-trips through save/load by STRING, and an old
  index-persisted value still applies (both directions of the robustness claim).
- unit (PanelCardDriver, via the test seam): options come from the seam with "none" first;
  picking an entry binds that NAME; a persisted name whose NIC is gone falls back to none +
  Warning without crashing.

## Verification

Desktop build zero warnings, ctest green. Live on macOS: the dropdown lists en0/en5 etc.,
picking one binds it (status shows the bind result), "none" returns to capture. The Windows
half (friendly names via Npcap) rides to the Windows tester with the next release, as raw-L2
itself did; the seam's structure is shared so only the enumeration source differs.

## Risks

- Friendly-name duplicates on Windows (two identical adapters): entries suffixed with the
  pcap name's tail when descriptions collide, so the Select rows stay distinct.
- Label persistence is a CORE format change for controls that opt in; opt-in keeps every
  existing Select's bytes unchanged.
