# Plan-18 — Release-channel picker + first-boot WiFi provisioning

> **Post-implementation note.** Plan-18 shipped all three tracks (OTA, web installer, Improv) as described. The branch carried six unplanned follow-up plans that landed in the same merge:
>
> - **Plan-19 — MoonDeck ESP32 tab refresh.** Replaced the dead "Chip" dropdown with a Firmware-variant picker; collapsed four "Build esp32-X" buttons into one parameterised Build; moved Setup above the dropdowns; separated Flash from Build with the Port dropdown between them; added a "destructive" confirm flag; fixed the `?` help-anchor renderer to emit `<h3 id="…">` so deep links land on the right section.
> - **Plan-19.1 — Per-target build directories.** `build/esp32-<board>/` + `build/<host>/`. Each board has its own build dir; switching boards is free (no clean rebuild). Mirrors the deployment layout the release workflow already used (`dist/firmware-<board>-v<ver>.bin`). A follow-up commit added `-DSDKCONFIG=…/sdkconfig` to keep per-build-dir sdkconfigs isolated from each other (`esp32/sdkconfig` at the project root no longer exists).
> - **Plan-20 — Web installer end-user features.** "Your devices" card backed by `localStorage`, with Visit / Erase / Forget buttons. Erase reuses ESP Web Tools' `erase-first` install button. Diagnose intentionally moved to the device UI (same-origin nav-footer link) because Chrome's mixed-content blocker prevents an HTTPS Pages page from fetching `http://<device>/api/state`.
> - **Plan-21 — Improv as a child of Network module.** Attempted, reverted same session. The architectural shape is right but crosses load-bearing infrastructure: `Scheduler::tick()` only walks top-level modules for `loop20ms`/`loop1s`, so a child module's tick callbacks silently disappear. Carved out for a future plan that fixes the scheduler/MoonModule chain first.
> - **Plan-22 — Nightly builds.** `.github/workflows/nightly.yml` cron'd at 04:00 UTC tags `nightly-YYYY-MM-DD` if `main` HEAD has moved since the last nightly, prunes nightly releases older than 7 days. Reuses `release.yml` via tag push — zero duplication of build matrix or Pages logic. `verify_version.py` learned to skip the library.json check on `nightly-*` tags (they're snapshot labels, not semver).
> - **Plan-23 — Split `platform_esp32.cpp` by subsystem.** 1281 lines → 700 (core) + 3 sibling files (FS, OTA, Improv). Network stayed in the core file because Eth + WiFi + sockets + mDNS share file-scope state — splitting would need an internal header with `extern` declarations or a singleton refactor. Desktop's `platform_desktop.cpp` deliberately stayed in one file; its OTA/Improv/FS stubs are 6 lines each. Asymmetry is intentional.
>
> The branch also carried a real bug fix the user surfaced mid-implementation: `NetworkModule::setWifiCredentials` did `wifiStaInit` directly without first stopping the AP-mode driver, so the `IP_EVENT_STA_GOT_IP` handler never registered and the state machine sat in limbo. AP→STA tear-down now runs explicitly in `setWifiCredentials`. The bug had been masked because the only callers were the credential-entry-then-reboot UI flow (reboot hides it) — Improv's "set credentials on a running device" flow exposed it.

## Context

projectMM has three installer surfaces, two of which don't exist in v3 yet:

1. **Web installer** at `https://ewowi.github.io/projectMM/install/` — first-flash, browser does the work via Web Serial. Bound to one release at deploy time today (plan-17's design).
2. **On-device OTA installer** — re-flash after the device is running. **Missing entirely in v3.** projectMM-v1 had it (`FirmwareUpdateModule` + `/api/firmware/url`).
3. **First-boot WiFi provisioning** — the flow that gets credentials onto a freshly-flashed device. Today it's "device boots SoftAP at `4.3.2.1`, user joins from a phone, opens the UI, types creds, reboots." Five friction steps, one of which (joining the AP from a phone) is genuinely confusing for non-technical users. **Missing in v3** — and v1 didn't have a polished version either; v1 used a deploy-time partition-baking script (`deploy/wifi.py` + `deploy/flashfs.py --wifi`) that's useful for racks of devices over USB but doesn't help an end user with one board.

Plan-18 builds all three with a coherent identity:

- **A shared release-channel picker JS module** powers both installers (Tracks 1+2): visitor (browser, or device-UI tab) picks **Stable** or **Pre-release (beta)** → release → board → click Install. Same code, two surfaces, one mental model.
- **Improv WiFi over USB-serial** (Track 3) handles first-boot provisioning. Browser drives the protocol immediately after a flash via ESP Web Tools; a Python CLI (`moondeck/build/improv_provision.py`) drives it for headless / rack / CI use. Same protocol, two transports.

**Step 0 of v1 of this plan empirically falsified plan-17 risk #4**: GitHub release-asset URLs (both `github.com/.../releases/download/` and the `release-assets.githubusercontent.com` redirect target) return **no `Access-Control-Allow-Origin` headers**. Cross-origin browser fetches are blocked. WLED works around this with a third-party CORS proxy (`proxy.corsfix.com`); ESPHome self-hosts every binary; projectMM-v1 sidestepped the problem entirely by not having a web installer. v3 chooses **self-host on Pages** (Option 1 from the CORS replan discussion): the release workflow stages the last 5 stable + 5 prerelease releases' binaries into Pages content. End-of-line origin = same as the install page, no CORS.

The OTA installer **does not have a CORS problem** — the device's ESP-IDF HTTPS client (`esp_https_ota`) has no Same-Origin Policy, just GETs the URL and writes bytes to the OTA partition. This asymmetry is why v1 has only the OTA flavour (easier). Plan-18 captures both: the *picker UX* is shared, the *binary fetch path* differs (browser-fetches-self-hosted vs device-fetches-GitHub).

Closed scope (locked by product owner):

- **Three tracks in one PR.** OTA → web installer → Improv, in that delivery order. All ship under plan-18.
- **Shared JS module at `src/ui/release-picker.js`**. Embedded into device builds via the existing `embed_ui.cmake` pipeline. Imported via `<script type="module">` in `docs/install/index.html` (same file, two consumers).
- **OTA compatibility filter**: device shows releases whose board is compatible with `MM_BOARD_NAME`. Bespoke rule, documented inline: strip `-eth*` suffix from both sides; matching identities are mutually compatible. So `esp32` / `esp32-eth` / `esp32-eth-wifi` are mutually compatible; `esp32s3-n16r8` is only itself.
- **Web installer CORS solution**: self-host last 5 stable + 5 prerelease releases on Pages. Release workflow stages binaries into the Pages artifact cumulatively (fetch-fresh-from-GitHub-Releases on each deploy; "Pattern D" from the Explore agent's research).
- **Smart default**: most-recent stable; fall through to most-recent prerelease if none exists.
- **RC visibility**: always shown in the channel dropdown, labelled "Pre-release (beta)". No URL gating.
- **Per-release board list**: derived from the release's `manifest-<board>.json` assets (strict regex parse).
- **Caching**: sessionStorage, 5-minute TTL. Dev escape hatch: `?nocache=1`.
- **Yanked releases**: delete the GitHub release; API stops returning it; cumulative-stage step trims it on next deploy. No special UI.
- **Two dropdowns: release + board.** Single flat release dropdown listing every release newest-first; RCs flagged with a `(beta)` suffix on the option text (e.g. `v1.0.0-rc1 (beta) — 12 hours ago`). Smart default selects the newest stable; falls through to the newest prerelease if none exists. **Reconciled mid-implementation**: the original plan called for a separate channel select + `<details>` "Pick specific release" expand, but the two-axis layout (stability vs version) was confusing; the single flat dropdown is simpler, the compatibility filter on the board step still does the protection work, and an RC remains visually distinct via the suffix + color.
- **Relative-time display** ("2 days ago") next to each release.
- **`app.js` becomes a module** (`<script type="module">`) to import `release-picker.js` cleanly. Single-attribute HTML change.
- **File-upload OTA route (`POST /api/firmware`) is skipped.** Picker drives `/api/firmware/url` only. v1 had both; v3 doesn't need the file-picker affordance on day one.
- **RC-tag Pages skip is removed.** With cumulative content, the install page is the canonical "all our releases" surface; Stable-by-default already protects naive end users. Pages publishes on every tag including RC.

Track 3 (Improv) closed scope:

- **Library**: `improv/improv` (v1.2.5) as an ESP-IDF managed component, fetched from the ESP Component Registry. ~10 KB of bundled library + cert-free serial protocol; reuses the existing mbedTLS bundle plan-18 already added for OTA. (Original plan said `improv-wifi/sdk-cpp` — that name was the GitHub repo coordinate, not the Registry coordinate; the Registry uses `improv/improv` and that's what ships in `idf_component.yml`.)
- **Serial source**: UART0 only on every board. ESP32-S3-DevKitC-1's UART USB port works (UART0 routed to the on-board USB-UART bridge); the S3's native USB-Serial-JTAG port doesn't. AP-fallback remains the only path for users with USB-CDC-only connections.
- **Lifecycle**: always-on listener, no task suspension. Provision requests are rejected (with Improv's wrong-state error frame) when `platform::wifiStaConnected() == true`; scan + info requests stay available so a browser can identify a running device.
- **What it surfaces**: one read-only `provision_status` Control matching `FirmwareUpdateModule`'s shape. No buttons, no re-provision affordance — the protocol is the entry point.
- **Rack / CI mode**: `moondeck/build/improv_provision.py` — pyserial CLI speaking the Improv protocol. Single-port mode today (`--port + --ssid + --password`); a future `--from-list <devicelist.json>` mode is a separate plan once v3 has a devicelist schema.
- **AP-fallback flow stays unchanged.** Improv adds a third credential-entry path alongside the AP fallback UI and the persistence-loaded values; all three converge on `NetworkModule::ssid_` / `password_`.

Prior art (the design isn't bespoke):

- **projectMM-v1 OTA**: `projectMM-v1/src/modules/system/FirmwareUpdateModule.h` (MoonModule with `update_status` + `update_pct` display controls), `projectMM-v1/src/core/OtaState.h` (file-scope statics), `projectMM-v1/src/core/AppRoutes.cpp:174-210` (`POST /api/firmware/url`), `projectMM-v1/src/frontend/app.js:1235-1410` (release-listing JS with sessionStorage cache + prerelease filter). Plan-18 ports the architecture, not the code — same shape, v3 idioms.
- **WLED installer at `install.wled.me`**: cross-origin release-asset fetch via `proxy.corsfix.com`. Plan-18 rejects this for the third-party dependency cost.
- **ESPHome at `web.esphome.io`**: self-hosts every binary in its Pages site. Plan-18's Track 2 follows this shape directly.

## Architecture

```text
                 src/ui/release-picker.js  (the shared module)
                       │
            ┌──────────┴────────────┐
            │                       │
    Device UI (OTA)             Web installer
    src/ui/app.js                docs/install/index.html
    imports inline               imports as <script type="module">
            │                       │
    Install click:              Install click:
    POST { url } to             setAttribute('manifest', url)
    /api/firmware/url           on <esp-web-install-button>
    (device fetches             (ESP Web Tools flashes via Web Serial)
    binary via HTTPS;
    no CORS)
            │                       │
            ▼                       ▼
    api.github.com           docs/install/releases/<tag>/*.bin
    /repos/.../releases      (Pages-hosted; release workflow
    (CORS OK for API)         stages new release + retains last
                              5 stable + 5 prerelease)
```

## Implementation steps

Two tracks, sequential within one PR. Track 1 must be hardware-verified before Track 2 touches the release workflow. Total estimated **~10 h**.

### Phase 0 — Shared groundwork (1.5 h)

**Step 0.1 — Read v1 prior art (0.5 h).** Read `projectMM-v1/src/frontend/app.js:1235-1410`, `projectMM-v1/src/modules/system/FirmwareUpdateModule.h`, `projectMM-v1/src/core/AppRoutes.cpp:174-210` end-to-end. Confirm sessionStorage shape, the `per_page` choice, relative-time rendering. Read-only.

**Step 0.2 — Add `hasOta` platform flag (0.25 h).** Add `constexpr bool hasOta = true;` to [src/platform/esp32/platform_config.h](src/platform/esp32/platform_config.h) and `= false;` to [src/platform/desktop/platform_config.h](src/platform/desktop/platform_config.h). Mirror the existing `hasWiFi` / `hasEthernet` pattern.

**Step 0.3 — Extend `mm::platform` with OTA primitives (0.75 h).** Add to [src/platform/platform.h](src/platform/platform.h) in the network section:

```cpp
// OTA — ESP32 only; desktop stubs return false from every entry point.
// Out-params drive Control buffers polled at 1 Hz by FirmwareUpdateModule.
bool ota_begin(size_t imageSize);
bool ota_write(const uint8_t* data, size_t len);
bool ota_end();
bool http_fetch_to_ota(const char* url,
                       char* statusBuf, size_t statusBufLen,
                       uint8_t* pctOut);
```

`ota_begin/write/end` are kept available even though the file-upload route is skipped for this PR — they're a clean abstraction over `esp_ota_*` and may serve a future use (a debug-only local-file-upload affordance, for instance).

### Track 1 — On-device OTA module (5.5 h, ships verified before Track 2)

**Step 1.1 — Build `src/ui/release-picker.js` (2.5 h, the load-bearing piece).** Create the new file. Self-contained ES module exporting one symbol:

```js
export const releasePicker = {
  init({ container, ownBoardKey, onInstall })
};
```

- `container`: DOM element to mount into.
- `ownBoardKey`: device's `MM_BOARD_NAME` (compatibility filter on); `null` = web installer (no filter).
- `onInstall(board, manifestUrl, binaryUrl)`: callback fired when user clicks Install. The picker does NOT decide *how* to install — caller's concern.

Sectioned the same way [src/ui/app.js](src/ui/app.js) is sectioned (hand-maintained, comment headers per section):

1. `fetchReleases()` — GET `https://api.github.com/repos/ewowi/projectMM/releases?per_page=10`, sessionStorage cache keyed on URL, 5-minute TTL. Returns normalised `[{tag, name, publishedAt, isPrerelease, assets:[{name,url}]}]`.
2. `parseBoardsFromAssets(assets)` — for each `manifest-<board>.json` asset, find the matching `firmware-<board>-v<ver>.bin` asset, return `[{board, manifestUrl, binaryUrl}]`.
3. `isCompatible(ownBoard, candidateBoard)` — strip `-eth*` from both; equal identities = compatible; null `ownBoard` = always compatible. **Bespoke**, one-line comment explains the rule.
4. `relativeTime(iso)` — `Intl.RelativeTimeFormat` + small diff-to-unit helper, ~15 lines, no library.
5. `render()` — channel select, release select (filtered by channel + compatibility), board select (per-release compatible boards), Install button. Smart default per the locked decisions.
6. Wire Install → `opts.onInstall(...)`.

**Common-patterns check**: ES-module shape, recognisable from Lit / vanilla web components. No bespoke framework choices.

**Risk**: medium. Single piece of load-bearing code for both tracks; a bug = bug everywhere. Mitigate with strict no-external-state design and identical sectioning to `app.js`.

**Step 1.2 — Wire the JS into the UI embedding pipeline (0.5 h).** Extend [src/ui/embed_ui.cmake](src/ui/embed_ui.cmake) to read `release-picker.js`, emit a `releasePickerJs[]` byte array + length. Add `release-picker.js` to the `DEPENDS` lists in both [CMakeLists.txt](CMakeLists.txt) (root) and [esp32/main/CMakeLists.txt](esp32/main/CMakeLists.txt). Add a `/release-picker.js` route to [src/core/HttpServerModule.cpp](src/core/HttpServerModule.cpp)'s GET ladder (one extra branch next to the existing `/app.js` route).

**Step 1.3 — `FirmwareUpdateModule.h` (0.5 h).** Create [src/core/FirmwareUpdateModule.h](src/core/FirmwareUpdateModule.h), header-only, mirrors v1's pattern + v3's [SystemModule.h](src/core/SystemModule.h) idioms:

- Two `controls_.addReadOnly` controls: `update_status` (char[64]), `update_pct` (uint8_t).
- File-scope statics `g_otaStatus[64]` / `g_otaPct` in an anonymous namespace inside the .h. Routes write these; `loop1s()` polls and copies into the bound buffers.
- `respectsEnabled() { return false; }` (diagnostics keep running).
- Registered in `main.cpp` alongside SystemModule.

**Step 1.4 — Platform OTA implementation (1.0 h).** In [src/platform/esp32/platform_esp32.cpp](src/platform/esp32/platform_esp32.cpp): implement the four functions from step 0.3.

- Include `<esp_ota_ops.h>`, `<esp_https_ota.h>`.
- `http_fetch_to_ota`: configure `esp_https_ota_config_t` with `esp_crt_bundle_attach` so api.github.com / objects.githubusercontent.com TLS works without a baked cert. Loop on `esp_https_ota_perform`, update `*pctOut` from `esp_https_ota_get_image_len_read` / total. **Blocking call** — the route in step 1.5 spawns a FreeRTOS task.

In [src/platform/desktop/platform_desktop.cpp](src/platform/desktop/platform_desktop.cpp): stub the four functions returning `false` and writing "unsupported" into `statusBuf`.

**Risk**: medium. `esp_https_ota_perform` task scheduling is the main hazard; getting the task pinned to a sensible core + stack size matters. Reference v1's `pal::http_fetch_to_ota` for the working numbers.

**Step 1.5 — `POST /api/firmware/url` route (1.0 h).** Add to [src/core/HttpServerModule.cpp](src/core/HttpServerModule.cpp). Gated `if constexpr (mm::platform::hasOta)`; desktop returns 501. Handler:

1. Parse JSON `{"url":"..."}` using the existing JSON-string extraction helpers (see `handleSetControl` patterns at line 428).
2. Validate URL shape (`http://` or `https://`).
3. `xTaskCreate` a one-shot task that calls `pal::http_fetch_to_ota`. Task updates `g_otaStatus` "downloading" → "flashing" → "rebooting", and `g_otaPct`. On success, `pal::reboot()`.
4. Return `202 Accepted` immediately. UI polls `update_status` via the existing WS state push.

**Risk callout — complexity**: HttpServerModule.cpp already carries 24+ lizard warnings (from plan-17). Keep the new function ≤20 lines; pull body-parsing into the existing pattern; don't refactor the surrounding GET/POST ladder. If lizard flags the new code as adding to the worst-offender list, split OTA routes into a separate `OtaRoutes.{h,cpp}` (deferred decision, only if needed).

**Skip**: file-upload route (`POST /api/firmware`) — picker drives URL-only. v1's file affordance is not part of this plan.

**Step 1.6 — UI wiring in app.js (0.5 h).** In [src/ui/index.html](src/ui/index.html), add `type="module"` to the existing `<script src="/app.js">` tag — single-attribute change. In [src/ui/app.js](src/ui/app.js):

- Top of file: `import { releasePicker } from "/release-picker.js";`
- In `createCard()` (around line 461-467), when `mod.type === "FirmwareUpdate"`, append a mount point and call:

```js
releasePicker.init({
  container: mount,
  ownBoardKey: getSystemBoard(),  // reads systemModule's `board` control
  onInstall: (board, manifestUrl, binaryUrl) =>
    fetch("/api/firmware/url", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ url: binaryUrl })
    })
});
```

The device uses the raw `.bin` URL, not the manifest — `esp_https_ota` ingests the firmware image directly. `ownBoardKey` reads from systemModule's `board` readonly control (already serialised in `/api/state` per `SystemModule.h:85`).

**Risk**: low. `<script>` → `<script type="module">` is a five-character change; modules' implicit strict mode shouldn't trip `app.js`. Modules `defer` by default; entry-point is async WS init at the bottom of app.js — no ordering surprises. Worth one deliberate page-load smoke test after the switch.

**Step 1.7 — Track 1 hardware verification (0.5 h, gating).** Flash the current build (`v1.0.0-rc1` or newer) to one esp32 and one esp32s3 board. On each: open the UI → FirmwareUpdate card. Confirm:

- Picker populates from api.github.com (within ~1 s, sessionStorage cache hit on reload).
- Releases dropdown shows only compatible builds: esp32 device doesn't see `esp32s3-n16r8` in the list; vice versa.
- Stable selected by default if any stable; else first prerelease (the current case — only `v1.0.0-rc1` exists).
- Install button advances `update_status` `idle` → `downloading` → `flashing` → `rebooting`; `update_pct` advances 0 → 100.
- Post-reboot, `board` control still reads the right value and the new firmware's version shows in `version`.

**Product-owner-driven; not agent-verifiable per CLAUDE.md.** Track 2 work does not begin until this is green.

### Track 2 — Web installer (3.0 h, ships after Track 1 hardware-verified)

**Step 2.1 — Adapt `docs/install/index.html` to use the shared module (0.5 h).** Edit [docs/install/index.html](docs/install/index.html):

1. Add `<script type="module" src="release-picker.js"></script>`. The file will be staged into Pages alongside `index.html` (step 2.3), so this is a same-origin import.
2. Replace the hardcoded `<select id="board">` and `renderButton()` JS (lines 103-185 minus the browser-warning check) with:

```js
import { releasePicker } from "./release-picker.js";
releasePicker.init({
  container: document.getElementById("picker-mount"),
  ownBoardKey: null,    // web installer flashes any board
  onInstall: (board, manifestUrl) => {
    const host = document.getElementById("button-host");
    host.innerHTML = "";
    const btn = document.createElement("esp-web-install-button");
    btn.setAttribute("manifest", manifestUrl);
    host.appendChild(btn);
    btn.click();
  }
});
```

Keep the SRI-pinned ESP Web Tools `<script>` tag and the browser-warning card.

**Step 2.2 — Per-release manifest URLs become relative (0.5 h).** [moondeck/build/generate_manifest.py](moondeck/build/generate_manifest.py) already accepts `--release-url`; passing `--release-url .` produces relative paths like `./firmware-esp32-v…bin`. This is the form needed for Pages-hosted manifests (same-origin). The current release-asset manifest stays absolute (used by the OTA picker which still passes binary URLs to the device).

Add a second invocation of the manifest generator in the workflow's "Generate ESP Web Tools manifests" step, writing a second set into `pages-manifests/`:

```bash
for B in esp32 esp32-eth esp32-eth-wifi esp32s3-n16r8; do
  python moondeck/build/generate_manifest.py \
    --board "$B" --version "$V" \
    --release-url . \
    --flasher-args "dist/flasher-$B.json" \
    --out "pages-manifests/manifest-$B.json"
done
```

**Step 2.3 — Cumulative release content staging (1.5 h, the genuinely risky bit).** Insert a new step in [.github/workflows/release.yml](.github/workflows/release.yml) before the existing "Stage GitHub Pages site" step:

```bash
# Last 5 stable + last 5 prerelease.
STABLE=$(gh release list --limit 50 --exclude-drafts \
         --json tagName,isPrerelease \
         | jq -r '.[] | select(.isPrerelease|not) | .tagName' | head -5)
PRE=$(gh release list --limit 50 --exclude-drafts \
      --json tagName,isPrerelease \
      | jq -r '.[] | select(.isPrerelease) | .tagName' | head -5)
KEEP="$STABLE $PRE"

for TAG in $KEEP; do
  mkdir -p "pages/install/releases/$TAG"
  gh release download "$TAG" --dir "pages/install/releases/$TAG" \
    --pattern 'firmware-*.bin' \
    --pattern 'manifest-*.json' \
    --pattern 'projectMM-*.tar.gz' \
    || true   # tolerate the current tag (assets not yet uploaded)
done

# The current release's binaries aren't a downloadable asset yet (the
# `softprops/action-gh-release` step runs *after* Pages staging). Stage
# them from the local dist/ + pages-manifests/ instead.
mkdir -p "pages/install/releases/${GITHUB_REF_NAME}"
cp dist/firmware-*.bin                       "pages/install/releases/${GITHUB_REF_NAME}/"
cp pages-manifests/manifest-*.json           "pages/install/releases/${GITHUB_REF_NAME}/"
cp dist/projectMM-*.tar.gz                   "pages/install/releases/${GITHUB_REF_NAME}/" || true
```

In the existing "Stage GitHub Pages site" step, add `cp src/ui/release-picker.js pages/install/` so the JS is alongside `index.html`.

**Remove the `if: !contains(github.ref, '-rc')` gates** on the three Pages-related steps (currently lines 197, 206, 229 of release.yml). Cumulative content needs every tag to refresh Pages or releases-vs-installer-view drift.

**Risk callout — release-process safety**: this step changes what lands on the live install URL on every tag. Mitigations:

- Test once via `workflow_dispatch` against an existing tag before merging.
- `--pattern` lists are explicit so a future release with surprise asset names doesn't pull in junk.
- After merge, confirm `releases/v1.0.0-rc1/manifest-esp32.json` is reachable at the live URL before considering Track 2 verified.

**Step 2.4 — Drop the separate `pages.yml` idea (0 h).** Plan-18 v1 proposed a separate `pages.yml` for docs-only changes. With cumulative content, a docs-trigger workflow buys nothing — it would have to re-run the same `gh release download` dance. Decision: don't create `pages.yml`; record in `docs/history/decisions.md` during PR-merge reconciliation.

**Step 2.5 — Track 2 verification (0.5 h, gating).** After the release workflow runs on a tag with the new shape:

- Visit `https://ewowi.github.io/projectMM/install/`. Picker populates from api.github.com.
- Pick a release + esp32 board → Install. DevTools Network: `manifest-esp32.json` fetched from `ewowi.github.io/projectMM/install/releases/<tag>/...` (same-origin). Three `.bin` files fetched from the same origin.
- No CORS errors in console. Flash succeeds against an unflashed ESP32.
- Switch channel to Stable / Pre-release; releases dropdown refreshes per filter.

**Product-owner-driven; not agent-verifiable.**

### Track 3 — Improv WiFi over USB-serial (3.0 h, ships after Tracks 1+2)

Browser-driven WiFi provisioning during/right after a first flash. Same browser session as the install, no SoftAP detour, no manual IP-hunting. ESP Web Tools speaks Improv natively, so Track 2's install page automatically offers the WiFi dialog once a firmware boots with Improv listening.

A small Python CLI mirrors the same code path for headless/CI/rack use over USB. Replaces v1's `deploy/wifi.py` + `deploy/flashfs.py --wifi` partition-baking flow (which required halting the device and re-flashing the LittleFS partition just to inject credentials).

**Step 3.1 — Add `improv/improv` as managed component (0.25 h).** Edit [esp32/main/idf_component.yml](../../esp32/main/idf_component.yml). Append:

```yaml
  improv/improv:
    version: "^1.2.5"
```

(The GitHub source is at `improv-wifi/sdk-cpp` but the ESP Component Registry coordinate is `improv/improv` — the latter is what `idf.py` resolves.)

(The two existing deps — `espressif/mdns` and `joltwallet/littlefs` — keep their format. Verify the exact registry name during implementation; if it's published as a different coordinate or only as a GitHub git source, switch to that form.) Run `idf.py reconfigure` locally before committing to confirm resolution.

**Risk callout — library API shape unverified at planning time.** The rest of Track 3 assumes a callback-driven library (parser eats bytes, emits callbacks for scan/info/provision). If the library is poll-driven, step 3.2 grows by ~30 min. The cache key at [release.yml:87](../../.github/workflows/release.yml#L87) is the IDF *toolchain* cache, not managed-components — no bump needed; the new component is fetched on first build.

**Step 3.2 — Platform-layer Improv listener (1.0 h).** Mirrors the OTA task pattern at [platform_esp32.cpp:870-891](../../src/platform/esp32/platform_esp32.cpp#L870-L891).

In [src/platform/platform.h](../../src/platform/platform.h), add to the network section after `wifiStaStop()`:

```cpp
// Improv WiFi provisioning over UART0. ESP32 only; desktop stub returns false.
// Always-on listener; the task installs a UART driver on UART0 and parses
// inbound Improv frames. Provision requests are rejected (with Improv's
// wrong-state error frame) while wifiStaConnected() is true; scan + info
// stay available. The callback is invoked from the Improv task with the
// new credentials; the module copies them and triggers wifiStaInit on the
// scheduler thread (avoids cross-task races).
using ImprovCredentialCallback = void(*)(const char* ssid, const char* password);
struct ImprovDeviceInfo {
    const char* name;            // borrowed; lifetime >= init call (statics are fine)
    const char* chipFamily;      // "ESP32" / "ESP32-S3" / ...
    const char* firmwareVersion; // kVersion from build_info.h
};
bool improvProvisioningInit(const ImprovDeviceInfo& info,
                            ImprovCredentialCallback cb,
                            char* statusBuf, size_t statusBufLen);
```

In [platform_esp32.cpp](../../src/platform/esp32/platform_esp32.cpp): anonymous-namespace state (callback ptr, status-buffer ptr+len, device info copy), an `improvTask` function that:
- Installs the UART driver on `UART_NUM_0` at 115200 (idempotent — the bootloader's pre-init is preserved for ESP_LOGI to keep writing).
- Loops on `uart_read_bytes(UART_NUM_0, buf, sizeof(buf), pdMS_TO_TICKS(100))`. Feeds bytes into the library's parser.
- On info request: reply built from the stored `ImprovDeviceInfo`.
- On scan request: synchronous WiFi scan via `esp_wifi_scan_start`; emit results.
- On provision: if `wifiStaConnected()`, status = `"error: already connected"` + Improv wrong-state error frame; else status = `"received credentials"` + invoke callback + 30s poll on `wifiStaConnected()` / `wifiStaGetIP()` for the final success/failure reply (with `http://<ip>/` URL in the success frame).
- `xTaskCreate(&improvTask, "improv", 4096, nullptr, 4, nullptr)`. 4 KB stack (no TLS); priority 4 (below OTA's 5, above idle).

In [platform_desktop.cpp](../../src/platform/desktop/platform_desktop.cpp): stub `improvProvisioningInit(...) { return false; }`.

**Risk callout — UART0 driver coexistence with ESP_LOGI.** In IDF v6 the log subsystem uses `esp_rom_printf` direct-to-register, and `uart_driver_install` claims only the interrupt — they coexist. Empirically confirm in step 3.7 by watching the serial monitor: ESP_LOGI lines should keep appearing after `improvProvisioningInit`. If they vanish, the fix is `esp_log_set_vprintf` to route logs through `vprintf`, ~30 min addition.

**Step 3.3 — Add `hasImprov` platform-config flag (0.1 h).** Mirror `hasOta` at [src/platform/esp32/platform_config.h:38](../../src/platform/esp32/platform_config.h#L38) (true on ESP32) and [src/platform/desktop/platform_config.h](../../src/platform/desktop/platform_config.h) (false). Call sites use `if constexpr (platform::hasImprov)` to compile out the listener-install path on desktop.

**Step 3.4 — `ImprovProvisioningModule.h` (0.5 h).** Header-only module at [src/core/ImprovProvisioningModule.h](../../src/core/ImprovProvisioningModule.h), mirrors [FirmwareUpdateModule.h](../../src/core/FirmwareUpdateModule.h)'s shape.

- One read-only Control: `provision_status` (char[64], default `"listening"`).
- `setSystemModule(SystemModule*)` + `setNetworkModule(NetworkModule*)` setters.
- `setup()`: gated `if constexpr (platform::hasImprov)`; builds `ImprovDeviceInfo` from `systemModule_->deviceName()` + `platform::chipModel()` + `mm::kVersion`; calls `platform::improvProvisioningInit(info, &onCredentialsThunk, statusStr_, sizeof(statusStr_))`. Updates `statusStr_` to `"listening"`.
- `onBuildControls()`: `controls_.addReadOnly("provision_status", statusStr_, sizeof(statusStr_))`.
- `loop1s()`: if `pendingCredentials_` flag set by the callback, copy `pendingSsid_` / `pendingPassword_` into the network module via a new `NetworkModule::setWifiCredentials(const char* ssid, const char* password)` public method, clear the flag.
- Static thunk + static `instance_` singleton bridge: the platform layer's C-style callback can't take a member pointer, so the module installs a free-function thunk that dispatches to `instance_->onCredentials(...)`. One-line comment at the introduction site documents the bespoke shape ("plain function pointer demanded by the C-style platform API; module is unique by construction").

Add `setWifiCredentials()` to [NetworkModule.h](../../src/core/NetworkModule.h):

```cpp
void setWifiCredentials(const char* ssid, const char* password) {
    if (!ssid) return;
    std::strncpy(ssid_, ssid, sizeof(ssid_) - 1);
    std::strncpy(password_, password ? password : "", sizeof(password_) - 1);
    markDirty();   // FilesystemModule notices and persists
    platform::wifiStaInit(ssid_, password_);
    // Existing state machine in loop1s() handles the 10s timeout + AP fallback.
}
```

**Step 3.5 — Wire into `src/main.cpp` (0.25 h).** Three edits matching the FirmwareUpdateModule registration pattern from [main.cpp:60](../../src/main.cpp#L60), [113-117](../../src/main.cpp#L113-L117), [174](../../src/main.cpp#L174):

1. `#include "core/ImprovProvisioningModule.h"` at the top.
2. `mm::ModuleFactory::registerType<mm::ImprovProvisioningModule>("ImprovProvisioningModule", "core/ImprovProvisioningModule.md");` in `registerModuleTypes()`.
3. Create + setters + `scheduler.addModule(...)` **after** `networkModule` (Improv depends on NetworkModule existing so its `setNetworkModule` setter has a valid pointer; the cold-boot order doesn't matter — both modules' `setup()` runs in the same scheduler phase).

**Step 3.6 — Python rack CLI: `moondeck/build/improv_provision.py` (0.5 h).** ~150-line pyserial script. Argparse:

```bash
improv_provision.py --port /dev/tty.usbserial-X --ssid <SSID> --password <PW> [--timeout 30]
```

- Open serial at 115200, send the Improv "send Wi-Fi settings" frame (RPC command 0x02, payload = `[ssid_len, ssid_bytes, pw_len, pw_bytes]`, then checksum).
- Loop reading frames until "provisioning success" (with URL) or "provisioning fail" (with error code) or timeout.
- Print: `provisioned <device> on <SSID>; UI at <URL>`. Exit 0 on success, non-zero on any error.

A future `--from-list moondeck/devicelist.json` mode for true rack provisioning is **out of scope for plan-18** — v3 doesn't have a devicelist schema yet; single-port mode covers single-device and "shell-loop a hub" today.

Add to [moondeck/MoonDeck.md](../../moondeck/MoonDeck.md) under a new "Provisioning" section. No MoonDeck button — the script is CLI-only by design.

**Step 3.7 — `docs/moonmodules/core/ImprovProvisioningModule.md` + hardware verification (0.4 h).**

Spec page sections (mirror FirmwareUpdateModule.md's shape, kept brief):
- One-paragraph "what Improv is" + link to <https://www.improv-wifi.com/>.
- Controls table (the one `provision_status` line — satisfies `check_specs.py`).
- **ESP32-S3 USB-port footnote**: explicit "connect to the silkscreen-labelled USB (UART) port, not USB (CDC/JTAG)" — the locked decision documented at the user-visible surface.
- "How to test": two paths — browser via <https://www.improv-wifi.com/> or ESP Web Tools' built-in Improv flow; CLI via `improv_provision.py`.

Hardware verification (product-owner gate):

1. Flash to an ESP32 and an ESP32-S3 DevKitC-1 (via the silkscreen UART USB port on the S3).
2. From Chrome desktop, open <https://www.improv-wifi.com/>, click Connect, pick the device's serial port. Expect device name + chip + version to appear in the browser; SSID scan returns nearby networks; enter creds → device shows `received credentials` → `connecting` → `connected: <ssid>`; URL `http://<ip>/` clickable; opens the device UI.
3. Same flow via the Python CLI: `python3 moondeck/build/improv_provision.py --port <port> --ssid <ssid> --password <pw>`. Expect exit 0 + the IP printed.
4. Confirm ESP_LOGI output still appears on the serial monitor throughout (the step 3.2 risk).
5. Wipe credentials (`POST /api/control` on `ssid` = empty + reboot) → confirm device boots AP-fallback as before. Re-run Improv via browser → confirm it provisions cleanly. The AP-fallback path stays intact.

Track 3 total: **3.0 h**. Sequential within the track. Hardware verification (step 3.7) is the gate; plan-18 doesn't ship without it green.

### Final housekeeping (0.5 h)

**Step 3.1 — `docs/install/README.md` recipes (0.25 h).** Simplify Recipe A to `cp docs/install/index.html /tmp/preview && cd /tmp/preview && python -m http.server 8000`. Mark Recipe B as rarely-needed (stub `loadReleases()` from DevTools for un-tagged-release testing). Update Recipe C with the new "Pages publishes on every tag (RC included)" mental model.

**Step 3.2 — `docs/plan.md` housekeeping (0.25 h).** Remove the "Installer with release-channel picker" stub (this plan supersedes it). Mention Phase 2 (nightly channel) + Phase 3 (UX polish) in plan-18.md's "Future phases" section so they're not lost.

## Critical files

**New:**

- [src/ui/release-picker.js](src/ui/release-picker.js) — shared release-picker module.
- [src/core/FirmwareUpdateModule.h](src/core/FirmwareUpdateModule.h) — on-device OTA MoonModule (header-only).
- [src/core/ImprovProvisioningModule.h](src/core/ImprovProvisioningModule.h) — Improv listener MoonModule (header-only). **Track 3.**
- [moondeck/build/improv_provision.py](moondeck/build/improv_provision.py) — pyserial CLI for headless / rack provisioning. **Track 3.**
- [docs/moonmodules/core/ImprovProvisioningModule.md](../../../moonmodules/core/moxygen/ImprovProvisioningModule.md) — spec page. **Track 3.**
- [docs/history/plan-18.md](docs/history/plan-18.md) — this plan's archive.

**Edited:**

- [src/platform/platform.h](src/platform/platform.h) — OTA primitives **+ `improvProvisioningInit` (Track 3)**.
- [src/platform/esp32/platform_config.h](src/platform/esp32/platform_config.h) + [src/platform/desktop/platform_config.h](src/platform/desktop/platform_config.h) — `hasOta` flag **+ `hasImprov` (Track 3)**.
- [src/platform/esp32/platform_esp32.cpp](src/platform/esp32/platform_esp32.cpp) — OTA implementation **+ Improv listener task (Track 3)**.
- [src/platform/desktop/platform_desktop.cpp](src/platform/desktop/platform_desktop.cpp) — OTA stubs **+ Improv stub (Track 3)**.
- [src/core/HttpServerModule.cpp](src/core/HttpServerModule.cpp) — `/api/firmware/url` route + `/release-picker.js` GET.
- [src/core/NetworkModule.h](src/core/NetworkModule.h) — new public `setWifiCredentials(ssid, password)` method (Track 3 — Improv writes through this).
- [src/ui/embed_ui.cmake](src/ui/embed_ui.cmake) — embed `release-picker.js`.
- [src/ui/index.html](src/ui/index.html) — `<script type="module">` for `/app.js`.
- [src/ui/app.js](src/ui/app.js) — import release-picker, wire to FirmwareUpdate card.
- [CMakeLists.txt](CMakeLists.txt) + [esp32/main/CMakeLists.txt](esp32/main/CMakeLists.txt) — embed deps.
- [esp32/main/idf_component.yml](esp32/main/idf_component.yml) — add `improv/improv` (Track 3).
- `main.cpp` — register FirmwareUpdateModule **+ ImprovProvisioningModule (Track 3)**.
- [docs/install/index.html](docs/install/index.html) — Track 2: use shared module.
- [.github/workflows/release.yml](.github/workflows/release.yml) — Track 2: cumulative content staging, remove RC-Pages gate.
- [moondeck/build/generate_manifest.py](moondeck/build/generate_manifest.py) — comment about the `--release-url .` use case.
- [docs/install/README.md](docs/install/README.md) — Track 2: simplify recipes.
- [moondeck/MoonDeck.md](moondeck/MoonDeck.md) — document `improv_provision.py` (Track 3).
- [docs/plan.md](docs/plan.md) — remove installer stub.

## Reuse map

| Source | Pattern to reuse | Why |
|---|---|---|
| [projectMM-v1 OTA module](../../projectMM-v1/src/modules/system/FirmwareUpdateModule.h) | MoonModule with two display controls + file-scope statics polled by `loop1s()` | Working pattern from v1. v3 ports the architecture using v3 idioms (`controls_.addReadOnly`, anon namespace statics). |
| projectMM-v1 `populateGhList()` | `fetch(api.github.com/.../releases) + sessionStorage cache + prerelease filter + per-asset install button` | Identical data shape; the picker UX is recognisable from v1. |
| projectMM-v1 `pal::http_fetch_to_ota` (`projectMM-v1/src/platform/esp32`) | `esp_https_ota_config_t` + `esp_crt_bundle_attach` + perform loop | Working ESP-IDF idiom. Cherry-pick the working numbers (stack size, core pinning) into v3. |
| [src/ui/embed_ui.cmake](src/ui/embed_ui.cmake) | The whole UI-embedding pipeline | Extend to one more file. No new architecture. |
| [src/core/SystemModule.h:85](src/core/SystemModule.h#L85) | `controls_.addReadOnly("name", buf, sizeof(buf))` for live-updating diagnostic strings | Same shape for `update_status` and `update_pct`. |
| [src/core/HttpServerModule.cpp:428+](src/core/HttpServerModule.cpp#L428) | `mm::json::parseString(body, "key", buf, sizeof(buf))` pattern | Reuse for parsing the `{"url":"..."}` body. |
| [moondeck/build/generate_manifest.py](moondeck/build/generate_manifest.py) | The whole script | Stays unchanged. Track 2 just calls it twice — once with absolute URLs (release assets) and once with `--release-url .` (Pages copy). |
| projectMM-v1 `deploy/wifi.py` + `flashfs.py --wifi` | The "one set of credentials, applied to a rack of devices" use case | The use case is preserved. The mechanism changes: v1 baked credentials into a LittleFS partition image and re-flashed it over USB (device halted). Track 3 talks Improv to running devices via UART — same end state, no flash required, generalises to any-firmware-with-Improv. |
| `improv/improv` (ESP Component Registry; source: `improv-wifi/sdk-cpp` on GitHub) | The Improv protocol parser + callbacks | Standard upstream library. We don't reimplement the protocol; we install the listener task that feeds it bytes. |
| [src/platform/esp32/platform_esp32.cpp:870-891](src/platform/esp32/platform_esp32.cpp#L870-L891) (`http_fetch_to_ota` task) | The xTaskCreate + status-buffer pattern | Improv's listener task is identical shape: heap struct + `xTaskCreate` + status-buffer ownership. Reuses the pattern, not the code. |

## Verification

Plan-18 passes Event-1 commit gates:

- Desktop build clean (`hasOta = false` + desktop OTA stubs return false; new C++ compiles).
- ctest + scenarios pass.
- Platform boundary passes (OTA primitives live in `src/platform/`, declared in `platform.h`).
- Spec check requires a new `docs/moonmodules/core/FirmwareUpdateModule.md` describing the two controls + the URL-fetch route — per CLAUDE.md "Module specs are end-user / API-integrator documentation."
- ESP32 build all 4 boards (the C++ changes affect ESP32 builds).
- KPI re-captured (new C++ in `src/`).

### Test coverage

Added under plan-18 (Option A — "cheap + extract parser"):

- **`test/test_improv_frame.cpp`** — 13 cases / 216 assertions over the Improv framing layer. Parser feed-byte-at-a-time, bad-checksum detection, oversize-length rejection, resync on garbage, the "stray 'I' restarts the magic search" edge case, builder/parser round-trip across all four frame types, back-to-back frames. The parser was extracted into [src/core/ImprovFrame.h](../../src/core/ImprovFrame.h) precisely to make this test cheap (no MCU, no `improv/improv` host port). The ESP32 task at [platform_esp32.cpp::improvTask](../../src/platform/esp32/platform_esp32.cpp) now consumes the same parser, so the unit test covers the bytes-in path that runs on-device.
- **`test/test_network_module.cpp`** — 4 cases on `NetworkModule::setWifiCredentials`: SSID + password copy, dirty-flag toggling, null-SSID no-op, null-password tolerance, oversize-SSID truncation. The desktop `wifiStaInit` stub returns false safely so the test runs without bringing up a radio. This is the bridge Improv uses to hand credentials to the network state machine.
- **`moondeck/build/improv_provision.py --self-test`** — host-side framing/payload round-trip. No serial port needed; re-runnable in CI. Catches a regression in the Python frame builder before any device is involved.

Documented gaps (not covered by automated tests, called out so reviewers know what hardware verification is buying):

- **Improv RPC dispatch on-device** — the bridge from a parsed frame to `improvHandleProvision` / `improvSendDeviceInfo` etc. lives in `improvDispatchFrame()` ([platform_esp32.cpp:1037](../../src/platform/esp32/platform_esp32.cpp#L1037)) and depends on the upstream `improv/improv` library (source: `improv-wifi/sdk-cpp`) plus real WiFi state. Covered only by Track 3.7 hardware verification.
- **`esp_https_ota` fetch + OTA flash + reboot** — the `urlOta` task in `http_fetch_to_ota`. Depends on the ESP-IDF TLS stack, OTA partition layout, and network. Covered only by Track 1.7 hardware verification.
- **`release-picker.js`** — the pure helpers (`isCompatible`, `parseBoardsFromAssets`, `relativeTime`) have been exercised ad-hoc from a DevTools console; there is no JS unit-test harness in v3 today (no jsdom, no node runner). Adding one is on the 2.0 roadmap (`docs/plan.md`).
- **Web installer CORS path + cumulative Pages content** — release-workflow change. Verified only by Track 2.5 hardware-flash test against the live `https://ewowi.github.io/projectMM/install/`.

Event-2 PR-merge gates apply normally. Reviewer agent's main checks: did the OTA route follow the existing HttpServerModule route patterns? Is the JS module's `isCompatible()` bespoke rule clearly documented at the introduction site?

Track 1, Track 2, and Track 3 hardware verification (steps 1.7, 2.5, 3.7) gate the plan; product owner runs all three.

## Risks and unknowns

1. **`esp_https_ota` TLS bundle**: api.github.com + release-assets.githubusercontent.com need `esp_crt_bundle_attach` (the standard ESP-IDF mechanism). Confirm during step 1.4 that the v6.1-dev IDF has it linked by default (it should; it's a baseline component).
2. **`xTaskCreate` stack size for the OTA task**: too small and the TLS handshake stack-overflows; too big and we waste RAM. v1's working number is the reference — cherry-pick.
3. **HttpServerModule.cpp lizard complexity**: plan-17 left it at 24+ warnings. The new route is one more entry in the if-ladder; should not push the file's worst-offender functions higher. If lizard flags a regression, the OTA routes split into `OtaRoutes.{h,cpp}` (deferred decision).
4. **JS module + page-load timing**: converting `app.js` to a module changes load timing (deferred by default). The WS init at the bottom of app.js is async anyway, but a smoke-test reload is mandatory after step 1.6.
5. **Cumulative content workflow drift**: the `gh release download` step in step 2.3 changes what lands on Pages on every tag. A bug here breaks the install URL for everyone. Mitigation: `workflow_dispatch` dry-run against an existing tag before tagging the next real release.
6. **`isCompatible()` correctness**: the bespoke rule (strip `-eth*` suffix, compare) is intentionally narrow. If we add a future board that doesn't fit this scheme (e.g. an ESP32-P4 variant), the rule needs updating. Documented inline so the next person sees it; tested implicitly by Track 1 verification.
7. **Improv library API shape unverified** (Track 3.1): callback-driven vs poll-driven changes step 3.2's task structure by ~30 min. The exact ESP Component Registry coordinate is also unverified at planning time.
8. **UART0 + ESP_LOGI coexistence** (Track 3.2): empirically OK in IDF v6, but verified only at the step 3.7 hardware test. Fallback (route logs through `vprintf`) is ~30 min if needed.
9. **ESP32-S3 USB-port confusion**: the DevKitC-1 has two USB ports; only the silkscreen-labelled UART one works with Improv. Documented in the spec page; user education, not a code fix.

## Notes

- This plan is saved as `docs/history/plan-18.md` once Track 1 implementation begins, per CLAUDE.md per-feature workflow.
- Track 1 = OTA picker. Hardware-verified before Track 2 work began. **(Done at planning time of Track 3 addition.)**
- Track 2 = web installer + release workflow changes. Implemented; ships in the same PR; the release workflow change is the riskiest single edit in Tracks 1+2.
- Track 3 = Improv WiFi + Python rack CLI. Added late in plan-18 to close the install-UX loop: the picker UI provisions firmware, OTA flashes new firmware, Improv provisions WiFi. Same PR.
- The cancelled v1 of plan-18 (which assumed cross-origin release-asset fetches worked) is the source of the CORS-gate context.
- Total plan-18 budget: Tracks 1+2 = ~10 h (done); Track 3 = ~3 h (new); total ~13 h. Sequential within tracks; product owner gates each track on hardware before moving to the next.
