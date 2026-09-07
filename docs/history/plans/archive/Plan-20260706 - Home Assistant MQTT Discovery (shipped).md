# Plan — Home Assistant MQTT Discovery (JSON schema)

## Context

A user (Shelly board) tried to control the device from Home Assistant's "Easy MQTT" UI addon and got
nothing: HA's addon publishes to HA's own schema (`homeassistant/light/<name>/set` + `{"state":"ON"}`
JSON), while the device subscribes to the mqttthing schema (`projectMM/<mac6>/on/set` + `"true"`). He
unblocked himself by hand-pointing HA at the device's real topics — proving this is an **ergonomics
gap, not a capability gap**. The device is *controllable* from HA today; it just isn't
*auto-discoverable*.

**What we build:** HA MQTT Discovery — the device announces itself via a retained
`homeassistant/light/<id>/config` topic so HA (and any Discovery-aware hub) auto-creates a
correctly-wired light entity. **JSON schema** (PO decision), matching the modern auto-discovery peer
group (Tasmota / ESPHome / Zigbee2MQTT) — *Common patterns first* — and chosen for **extensibility**:
a JSON light carries all state in one atomic message and has native `effect`/`effect_list`, so future
controls (presets, effects, palette-as-color) add a key, not a new topic + custom HA config. The
existing `projectMM/<mac6>/…` mqttthing topics stay byte-identical (the user's working setup is
untouched); Discovery lands *alongside* them.

Feature branch `ha-mqtt-discovery` (already created; carries the earlier backlog-priority edit).

## Decisions locked (PO)

- **JSON schema** discovery, not default schema.
- **`unique_id` = `<last6-MAC>`** (the stable id per [ADR-0010](../../../adr/0010-integration-identity-stable-hardware-id.md)), `name` = `SystemModule::deviceName()`. Never the editable name as identity.
- Gated on a new **`haDiscovery`** bool control (default on where MQTT ships); toggling re-announces / retracts.
- Existing mqttthing topics unchanged; Discovery is additive.

## Design (reuse the existing MqttModule seams)

All in `src/core/MqttModule.{h,cpp}` + `src/core/MqttPacket.h`, on `loop1s()` (off the hot path). The
Explore map confirmed every primitive needed already exists; the work is three small additions.

**1. Announce (on CONNACK-accept, near the existing `publishName()`/`publishState(true)` at ~cpp:253):**
- New `buildDiscoveryTopic(out, cap)` → `homeassistant/light/<mac6>/config` (independent of
  `topicPrefix()`/`buildTopic()`, which hard-code the `projectMM` root).
- New `publishDiscovery(bool announce)` — builds the retained JSON config and publishes it with the
  existing `buildMqttPublish(topic, payload, len, buf, len, /*retain=*/true)` (already supports
  arbitrary topic + retain). A retracting empty retained payload when `haDiscovery` is toggled off.
- **Config JSON** (JSON-schema light; validated field-by-field against HA's light.mqtt JSON schema —
  `schema:"json"` required, `cmd_t`/`stat_t`/`uniq_id`/`avty_t` are the correct abbreviations,
  `"brightness":true` enables brightness at the default 0-255 scale, `dev{ids,name,mf,mdl}` shape
  correct):
  `{"schema":"json","name":"<deviceName>","uniq_id":"projectMM_<mac6>","cmd_t":"projectMM/<mac6>/ha/set",
  "stat_t":"projectMM/<mac6>/ha/state","avty_t":"projectMM/<mac6>/status","brightness":true,
  "dev":{"ids":["projectMM_<mac6>"],"name":"<deviceName>","mf":"MoonModules","mdl":"projectMM"}}`.
  (`uniq_id` prefixed `projectMM_` for cross-vendor uniqueness. Dedicated `ha/set` + `ha/state` keep
  the JSON-schema traffic separate from the scalar mqttthing `on/set` etc. — no topic carries two
  payload formats.)
- **Buffer (P0 — silent-failure risk):** the config PUBLISH is ~285 bytes (payload ~247 + topic 33),
  **over `kSendBufLen = 256`** — and `buildMqttPublish` returns 0 on overflow while `sendPacket(buf,0)`
  returns true, so the config would **never send with no error**. Use a **dedicated `uint8_t
  buf[384]`** in `publishDiscovery()` (not a global `kSendBufLen` bump, which fattens every per-tick
  frame), and **guard the `n == 0` return** as a real failure.

**2. Inbound (new branch in `routePublish`, ~cpp:262):**
- Match suffix `ha/set`, payload `{"state":"ON"|"OFF"[,"brightness":0-255][,"effect":"..."]}`.
- **Reuse `mm::json` (`src/core/JsonUtil.h`) — do NOT hand-roll.** The repo already has flat
  `json::hasKey/parseBool/parseInt/parseString`, and `HttpServerModule::applyWledState()`
  (HttpServerModule.cpp:1074-1087) is the exact precedent: it parses an inbound `{"on":…,"bri":…}`
  body with those helpers (0-255 clamp) and routes via `setControl("Drivers",…)`. `ha/set` is the same
  shape (`state` is a string → `json::parseString`). The helpers tolerate HA's whitespace and are
  key-order-independent. (NOT the heavy recursive `json::parse`/arena — that's for the nested device
  list.) The inbound body needs a **larger local buffer than the existing `value[32]`** (cpp:271).
- Maps to the same `setControlValue("on"/"brightness", …)` calls — HA JSON brightness is 0–255, so
  **no rescale** (default `brightness_scale` is 255).
- Extensible: an `effect` key later → `setControlValue("palette"/"effect", …)` with `effect_list` in
  the config.
- SUBSCRIBE to `ha/set` when `haDiscovery` is on — at CONNACK (alongside `kSets[]`, cpp:241) AND on a
  mid-session toggle-on (subscriptions today happen only once at CONNACK).

**2b. Availability (LWT — industry standard, cheap; PO: include).** Every serious MQTT device
(Tasmota/ESPHome/Zigbee2MQTT/Shelly) backs an availability topic with an MQTT Last-Will, so HA greys
the entity out the moment the device drops — the exact "dead light shows as on" bug Discovery must not
leave. Cost is small and off the hot path: extend `buildMqttConnect` (MqttPacket.h:124) with optional
**will-topic / will-payload / will-retain** (3 connect-flag bits + two more strings, same mechanism as
username/password already there; ~25 lines + a golden-vector test). Declare the will = retained
`offline` to `projectMM/<mac6>/status`; publish retained `online` to the same topic on CONNACK-accept;
add `"avty_t":"projectMM/<mac6>/status"` to the discovery config. The broker publishes `offline` on an
ungraceful drop — no polling/timers on our side. Zero hot-path / memory cost.

**3. State (extend `publishState`, ~cpp:341):**
- The existing change-gate (on/bri/palette vs `last*`) already fires "on change." Add, when
  `haDiscovery` is on, a retained publish to `ha/state` of `{"state":"ON|OFF","brightness":<0-255>}`
  (HA-scale brightness, no rescale) **inside that gated block** (not unconditional per tick — so an
  inbound `ha/set`→setControl→publishState emits once, no loop). Retained so a late-joining HA gets
  current state. One more `publish(...)` alongside the three `*/get` topics.

**4. Control + wiring:**
- `addBool("haDiscovery", haDiscovery_)` in `onBuildControls` (~cpp:49), member default
  `haDiscovery_ = true` (opt-out; `addBool` has no default-arg, and there's no per-deviceModel hook at
  this layer). Retract-on-disable = an empty retained payload to the config topic
  (`buildMqttPublish(…, 0, …, retain=true)` handles `payloadLen==0`).
- `onUpdate` (~cpp:62): the `haDiscovery` arm must **NOT `resetConnection`** (that bounces the socket).
  Instead call `publishDiscovery(on/off)` directly, and on a mid-session turn-ON also SUBSCRIBE to
  `ha/set` — announce/subscribe live, no reconnect.

## Files

- **Edit:** `src/core/MqttModule.h` (`haDiscovery_` field, new method decls), `src/core/MqttModule.cpp`
  (announce + inbound branch + state + control + subscribe), `src/core/MqttPacket.h` *(only if a
  discovery-config helper is cleaner there; likely not — `buildMqttPublish` suffices)*.
- **Tests:** `test/unit/core/unit_MqttPacket.cpp` — golden-vector (a) the CONNECT packet **with the
  Last-Will** (byte-exact, the will-flag bits + will topic/payload), and (b) the retained
  discovery-config PUBLISH (byte-exact topic + JSON + retain bit) — the pattern the `name` retain test
  already uses. `test/unit/core/unit_MqttModule.cpp` — feed `ha/set` `{"state":"OFF"}` /
  `{"brightness":128}` via `Rig::publish` and assert `FakeDrivers.on/brightness` (existing
  effect-assertion style). For "CONNACK → module emits the retained config", add a **test-only capture
  buffer in `sendPacket`** (the cleanest fit for the `feedForTest` pattern; `conn_` is concrete, a fake
  socket is heavier than the module's conventions) and assert the captured config bytes.
- **Docs:** `docs/moonmodules/core/MqttModule` `///` (the topic list gains the `ha/set`/`ha/state` +
  the discovery announce); `docs/usecases/home-automation.md` (an HA-via-Discovery recipe: "it just
  appears" — plus keep the manual-topic + WLED paths); `docs/backlog/backlog-core.md` (the "HA MQTT
  Discovery" item ships → delete it, per *Mandatory subtraction*).

## Extensibility (the PO's question, answered in the design)

JSON schema grows by adding a key to the config + the state/command JSON — no new topic, no new HA
entity type. Concretely: **presets/effects** → `effect_list:[…]` in the config + `{"effect":"Fire"}`
on the wire (HA renders a dropdown natively); **color** (when palette→color matures) →
`{"color":{"h":…,"s":…}}`. The default schema has no `effect` support at all — this is the deciding
reason for JSON.

## Verification

1. `cmake --build build` clean; `ctest` (the new golden-vector + inbound tests) + scenarios green;
   `check_specs.py` green.
2. Byte-exact: the discovery-config PUBLISH matches the golden vector (topic
   `homeassistant/light/<mac6>/config`, retain bit set, the JSON payload).
3. **HW (PO):** flash a WiFi board, set broker + `haDiscovery` on → HA auto-creates the light entity
   (no manual YAML) → toggle on/off + brightness from HA, confirm the strip responds and HA reflects
   state on device-side change. Toggle `haDiscovery` off → the entity disappears (retained config
   retracted). Confirm the existing `projectMM/<mac6>/on/set` mqttthing path still works unchanged.
4. Platform boundary: all socket I/O via the existing `platform::TcpConnection`; no new platform code.

Save the approved plan to `docs/history/plans/Plan-20260706 - Home Assistant MQTT Discovery.md`.
