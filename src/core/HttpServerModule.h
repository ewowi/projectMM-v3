#pragma once

#include "core/MoonModule.h"
#include "core/TryLock.h"   // the cross-thread sender latch (wsLock_)
#include "core/ScratchBuffer.h"   // leafHashes_ — the growable diff-on-the-wire value-hash cache
#include "core/BinaryBroadcaster.h"
#include "platform/platform.h"

#include <cstdint>

namespace mm {

// Forward declarations — bodies in HttpServerModule.cpp include the real headers.
class JsonSink;
class Scheduler;

/// Embedded HTTP server plus WebSocket — serves the web UI and the REST API that backs it.
/// Core infrastructure held to a **light-include-free** contract with one PO-accepted
/// exception: the WLED-compatibility shim's color path uses `light/Palette.h`'s pure
/// hue/RGB↔palette-index conversions (`Palettes::nearestForRgb`, `Palettes::representativeRgb`),
/// the same sanctioned exception `MqttModule` documents at its top-of-file — routing a HomeKit /
/// HA WLED color to a projectMM palette needs the palette set, which is inherently light-domain,
/// and a format conversion is the least-coupling way to bridge it (this module still drives the
/// palette through `Scheduler::setControl`, not a light object). No other light-domain include
/// is permitted here. Implementation lives in HttpServerModule.cpp; this header is the interface
/// only. The `port` control defaults to 8080 on desktop, 80 on ESP32.
///
/// **REST API:** `GET /` serves index.html and the UI assets (`/app.js`, `/style.css`,
/// `/moonlight-logo.png`). `GET /api/state` returns the full module-tree JSON (each entry
/// carries name, type, role, enabled, tickTimeUs, classSize, dynamicBytes, `controls[]`,
/// status + severity when set, and `userEditable:false` only when the module opts out of
/// UI delete/replace). `GET /api/system` returns fps, tickTimeUs, freeHeap, freeInternal,
/// maxBlock, uptime. `GET /api/types` returns the type catalog (stable factory `name`,
/// role-suffix-stripped `displayName`, `acceptsChildRoles`, and per-type `defaults` captured
/// from a fresh probe instance). Mutations: `POST /api/control` `{module,control,value}`,
/// `POST /api/modules` create, `POST /api/modules/{name}/move` reorder, `.../replace` swap,
/// `POST /api/reboot`, `DELETE /api/modules/{name}`. File Manager: `GET /api/dir?path=` lists a
/// directory, `POST /api/dir?path=` creates a folder, `DELETE /api/dir?path=` removes a file or
/// empty folder, `GET|POST /api/file?path=` reads / writes a file body (the path rides the query,
/// so a filesystem op carries its target in the request, not a stored control). All JSON responses
/// stream through a `JsonSink` — no fixed-buffer ceiling, so a tree of any size serializes correctly.
///
/// **WebSocket:** `GET /ws` with `Upgrade: websocket` does the RFC 6455 handshake (SHA-1 +
/// base64). Two WS channels by traffic class, with separate caps on one lwIP socket budget:
/// `/ws` carries the control plane (JSON state and patches, `MAX_WS_CLIENTS` = 8) and `/wsp` the
/// lossy binary preview stream (`MAX_PREVIEW_CLIENTS` = 4). Every binary message takes ONE path:
/// the resumable buffered send (`sendBufferedFrame`), draining a memory-adaptive chunk per client
/// per `tick20ms` from a stable caller-owned buffer, so a large frame is delivered over
/// wall-clock ticks without any loop ever waiting on a socket, yet stays one atomic WS message.
/// One buffered send is in flight at a time per slot (newest-wins backpressure: a new offer while
/// one is active is dropped); a client is closed only on a real error or FIN, never for slowness.
/// Inbound `/wsp` payloads are unmasked and handed opaquely to the registered producer sink; the
/// producer's vocabulary is `[0x51][stride][fps]` (standing frame request) and `[0x52][stride]`
/// (one-shot table request). Other mutations go through REST.
///
/// **State push — diff on the wire (the recognizable snapshot-then-patch model, cf. Redux /
/// Firestore sync, JSON Patch RFC 6902):** the state a client needs is the full module tree
/// (~30 KB, mostly *unchanging* option/detail metadata), but re-serializing all of it every
/// `tick1s()` — inline on the render thread — stole render budget and stuttered the LEDs at 1 Hz.
/// Instead: a client gets the **full** `{modules:[…]}` state ONCE on connect (chunk-drained via
/// the resumable sender, off the render tick), then each second a **patch** `{patch:[{path,value},
/// …]}` of only the values that changed. Change is found by value-compare, not a dirty flag:
/// `buildStatePatch` serializes each leaf's value, hashes it (FNV-1a), and compares to a cached
/// hash — so a value the device mutates itself (telemetry `@tickTimeUs`, status, a driver) is
/// caught the same as a `setControl` write, with no per-write instrumentation. A leaf path is
/// `"<module>/<control>"` (or `"<module>/@<field>"` for live per-card header telemetry); module
/// names are unique tree-wide, so the path is stable. The hash cache is one global baseline (not
/// per-client) in a growable `ScratchBuffer<LeafHash>`; `requestFullResync()` re-sends the full
/// state + re-baselines on connect and after any structural change (a value patch can't describe
/// a reshaped tree). A **schema change** (a `rebuildControls()` from any trigger — a control set, a
/// list mutation, an async WiFi/Hue callback) also forces a resync via a static schema-changed hook
/// (`MoonModule::setSchemaChangedHook`), since the value patch can't carry changed hidden flags /
/// option sets. A pending resync is fast-pathed on `tick20ms` (not just `tick1s`) and **preempts**
/// an in-flight preview frame, so a freshly-connected client gets its state — and therefore its
/// preview — within a few tens of ms instead of up to a second. Net: the per-second push drops from
/// ~34 KB to ~1–2 KB and the expensive full-tree serialize runs only on connect / schema change. The
/// UI applies a patch in place (no rebuild) and re-renders on a full frame. This is the "sub-hot path
/// is a hot path" rule (CLAUDE.md) applied: a periodic tick shares the render thread, so its work
/// must be cheap / skipped-when-unchanged.
///
/// **Hot-path split:** the resumable drain runs on `tick20ms` (the 20 ms transport poll),
/// deliberately NOT the per-render-tick `tick()`, so pushing preview bytes to the socket is
/// never charged to the LED render hot path. The LED output is never delayed by the preview;
/// the preview frame rate is instead bounded by the 20 ms drain cadence, which is the right
/// trade since the preview is a view and the LEDs are not.
///
/// **WLED-compatibility shim:** a small set of WLED-shaped messages make a projectMM device
/// appear in — and be controlled from — the native WLED apps (iOS / Android) and Home
/// Assistant's WLED integration. Discovery is over mDNS `_wled._tcp`; validation is a minimal
/// `GET /json/info` `{name, mac, leds{}, wifi{}, brand:"WLED", product:"MoonModules"}` (the
/// app keys on `brand:"WLED"` to accept it — we interoperate, not impersonate; this is NOT a
/// full WLED emulation). Live state is pushed over `/ws` as a `{state, info}` frame; `state`
/// mirrors the Drivers `brightness` control and the live first-LED RGB (falling back to
/// projectMM purple `[128,0,255]` when the first LED is off). Control is bidirectional over the
/// same `/ws`: the app's slider/toggle send a `{on?, bri?}` frame, read by
/// `pollWledStateFromWebSockets()` and applied to Drivers brightness through the shared
/// apply-core (the same `applySetControl` path REST and Improv use). The color read is the
/// one place this core module reaches output state — `MoonModule::firstOutputRgb()` is a
/// domain-neutral virtual the light-domain Drivers overrides — keeping this module free of any
/// light-domain include.
///
/// **Cross-domain wiring:** this module exposes the `BinaryBroadcaster` interface; the
/// light-domain PreviewDriver holds a `BinaryBroadcaster*` and streams each frame's bytes
/// through it. `main.cpp` wires PreviewDriver's broadcaster to the HttpServerModule instance —
/// the only file that knows both. The preview's point budget and wire format are PreviewDriver's
/// concern.
///
/// The five `JsonSink&` helpers below are private members rather than free functions because
/// they all read `this->wsClients_`, `this->scheduler_`, or other module state, or call other
/// HttpServerModule members. Three pieces of this module's helpers live in their own headers:
/// JsonSink + jsonEscape() in core/JsonSink.h, sha1() (RFC 3174, WS handshake) in core/Sha1.h,
/// base64Encode() (WS handshake + Password obfuscation) in core/Base64.h — all in `namespace mm`
/// so the call sites are unchanged.
///
/// **Prior art:** the WLED-compatibility shim's exact field requirements were reverse-engineered
/// from the WLED-Android client by Christophe Gagnier (@Moustachauve,
/// https://github.com/Moustachauve/WLED-Android) — `DeviceDiscovery.kt` (mDNS browse),
/// `DeviceFirstContactService.kt` (the `/json/info` validation + non-empty `mac` check), the
/// Info/State Moshi models, and `WebsocketClient.kt` (live state over `/ws`, the `sendState`
/// control direction). Knowing precisely what the app reads is why the shim is the minimal
/// accepted object rather than a guessed full WLED emulation.
class HttpServerModule : public MoonModule, public BinaryBroadcaster {
public:
    uint16_t port = 8080;

    void setScheduler(Scheduler* s) { scheduler_ = s; }
    void setUiPath(const char* path) { uiPath_ = path; }

    /// BinaryBroadcaster — stream one binary WS frame to every connected client, pushed
    /// incrementally so no frame-sized buffer is held. Producers (PreviewDriver) push the
    /// payload bytes; this prepends the WS header. Domain-neutral: no knowledge of the content.

    /// Resumable one-frame send from a stable caller-owned buffer (no copy), drained a bounded chunk
    /// per client per tick20ms (drainPreviewSend) so a large frame stays off this module's hot path;
    /// a would-block socket resumes next tick. See BinaryBroadcaster.
    bool sendBufferedFrame(const uint8_t* header, size_t headerLen,
                           const uint8_t* body, size_t bodyLen) override;
    bool bufferedSendIdle() const override { return !previewSend_.active; }
    // Drop the in-flight buffered preview frame (a geometry rebuild is about to free its body).
    // A client that already received part of the message has a desynced stream if we just stop -
    // the next message's bytes get parsed as this one's payload, so the only honest exit for a
    // mid-frame client is CLOSE (it reconnects and gets the fresh table). Untouched clients keep
    // their connection.
    void cancelBufferedSend() override {
        if (previewSend_.active) {
            const size_t total = previewSend_.hdrLen + previewSend_.bodyLen;
            for (int i = 0; i < MAX_PREVIEW_CLIENTS; i++)
                if (previewClients_[i].valid() &&
                    previewSend_.sent[i] > 0 && previewSend_.sent[i] < total) {
                    previewClients_[i].close();
                    // Every close site notifies the producer, or the dead slot's standing
                    // request would keep steering the shared stream until the slot is reused.
                    if (clientSink_) clientSink_->onClientGone(i);
                }
        }
        previewSend_.active = false;
    }


    int subscriberCount() const override {
        int n = 0;
        for (const auto& pc : previewClients_) if (pc.valid()) n++;
        return n;
    }

    /// Register the producer that receives this channel's inbound client messages (opaque bytes).
    void setClientMessageSink(ClientMessageSink* sink) override { clientSink_ = sink; }

    /// Parse ONE masked client data frame (text/binary, payload up to 8 bytes) from a /wsp read,
    /// unmasking the payload into `out`. Returns the payload length (>=0) or -1 when the buffer
    /// holds no complete parseable frame; `consumed` receives the whole frame's byte length so a
    /// caller can walk a read that coalesced several frames (0 when nothing was consumed). The
    /// payload's MEANING belongs to the registered sink; this only does RFC 6455 framing.
    /// Pure and static so the byte handling is unit-testable without a socket.
    static int parsePreviewUplink(const uint8_t* buf, int n, uint8_t out[8], int* consumed);

    // The cross-core sender lease (see BinaryBroadcaster). Guards previewSend_ + the wsClients_ socket
    // writes against this module's own core-0 drain / state push while an offloaded PreviewDriver
    // streams from core 1. try_lock: a busy transport returns false and the producer skips its frame.
    bool tryAcquireSend() override { return wsLock_.tryAcquire(); }
    void releaseSend() override { wsLock_.release(); }

    /// Keep running even when "disabled" via the UI — otherwise the user has no way
    /// to re-enable themselves through the same UI.
    bool respectsEnabled() const MM_NONBLOCKING override { return false; }

    /// Non-UI: this IS the server that renders /api/state — it doesn't list itself as a card.
    /// The "not a UI module" opt-out (shared with FilesystemModule), read by the state serializer's
    /// module loop to skip this module.
    bool appearsInUi() const override { return false; }

    void defineControls() override;
    void setup() override;
    void release() override;
    void tick20ms() MM_NONBLOCKING override;
    void tick1s() MM_NONBLOCKING override;

    // -----------------------------------------------------------------------
    // Transport-free apply-core — "the REST API, callable in-process"
    // -----------------------------------------------------------------------
    /// The add/set/clear-children operations the HTTP handlers do, factored out of
    /// the TcpConnection so any transport can drive them. Two callers today: the
    /// HTTP handlers (thin wrappers that map OpResult → status code) and the Improv
    /// serial path (ImprovProvisioningModule applies a pushed op on the main loop —
    /// "Improv = REST over serial"). One home for the apply logic; transports differ
    /// only in how they frame the request and report the result.
    enum class OpResult : uint8_t {
        Ok,
        AlreadyExists,   ///< add is a no-op: a module with this id is already in the tree (still success)
        ModuleNotFound,  ///< module / parent name not in the tree
        ControlNotFound, ///< module exists but has no such control (a distinct 404)
        UnknownType,     ///< factory doesn't know the type
        BadRequest,      ///< missing field, top-level add, parent rejected child
        OutOfRange,      ///< numeric value outside bounds
        Malformed,       ///< value didn't parse (such as an IPv4)
        ReadOnly,        ///< tried to write a display-only control
    };
    /// body is a small JSON object: `{"type","id","parent_id"}` / `{"module","control","value"}`.
    // outName (optional): on OpResult::Ok, receives the created module's FINAL name (after
    // ensureUniqueName disambiguates a collision) so a client can select/focus it. Pass its
    // buffer size in outNameLen. Null → not reported (the APPLY_OP transport doesn't need it).
    OpResult applyAddModule(const char* typeName, const char* id, const char* parentId,
                            char* outName = nullptr, size_t outNameLen = 0);
    OpResult applySetControl(const char* moduleName, const char* controlName, const char* valueJson);
    /// Enumerate-then-DELETE every child of `parentName` (the catalog inject's
    /// replaceChildren). Returns NotFound if the parent doesn't exist, else Ok.
    OpResult applyClearChildren(const char* parentName);
    /// Parse a single REST op object (`{"op":"add|set|clearChildren", …}`) and dispatch
    /// to the three above. The wire shape the Improv APPLY_OP frame carries.
    OpResult applyOp(const char* opJson);

    /// A file at `path` changed (written or removed), so ask the tree to re-derive whatever was
    /// built from it.
    ///
    /// The rule core already enforces for the OTHER way persistent state changes: applySetControl
    /// ends in the same request. A file is the second path to it, so it belongs here rather than
    /// in whichever client remembers to send a follow-up nudge (curl and MoonDeck would not).
    /// Transport-free like the apply-core above, and for the same reason: it is provable without a
    /// socket, and any future write path (a serial upload) reaches one implementation.
    ///
    /// Whole-tree rather than a path-to-module registry, which would need an association nothing
    /// else in the system keeps. A module decides for ITSELF whether what it holds actually
    /// changed: prepare() is the cold path that exists to be re-entered, and a scripted module
    /// compares a 4-byte content hash rather than re-reading its source.
    ///
    /// `path` is accepted for the diagnostics and for a future narrower dispatch; today every
    /// successful write asks the same question, so it is deliberately unused.
    void applyFileChanged(const char* path);

    /// Decode a `path=<rel>` query value into `out` (%XX + '+' decoding), rooted at the mount.
    /// Returns false on a missing/empty path, a `..` traversal, or an overlong (buffer-filling)
    /// value. The single filesystem-path guard shared by every fs HTTP entry (read/write/dir/
    /// mkdir/delete). Public + static so it's unit-testable without a socket fixture.
    static bool parseFilePath(const char* query, char* out, size_t cap);

    /// Case-insensitive substring search for a header name in a raw request (RFC 9112: field
    /// names are case-insensitive: browsers send "Content-Length:", node's undici sends
    /// "content-length:"; the case-sensitive strstr it replaces silently read a length of 0 and
    /// committed EMPTY files with a 200). Public + static so it's unit-testable without a socket.
    static const char* findHeaderCI(const char* hay, const char* needle);
    /// What a replaced module should be called: requested name, else a custom one, else null
    /// ("keep the fresh module's own default"). Static and public so the rule is unit-testable:
    /// it decides what a card is labeled after a swap, which is not something to discover in the UI.
    static const char* replacementName(const char* requested, const char* current,
                                       const char* oldDefault);

    /// Apply a WLED `{on?, bri?}` state body onto the Drivers `on` / `brightness` controls through
    /// the shared apply-core (`on` and `bri` independent — off preserves the level). The transport-
    /// free entry the HTTP `POST /json/state`, the inbound-`/ws` path, and the unit tests all drive.
    void applyWledState(const char* body);

    /// Test seams for the diff-on-the-wire patch: drive buildStatePatch / baseline / resync directly
    /// (they're otherwise private, called from tick1s). A unit test needs no socket to prove the diff.
    uint16_t buildStatePatchForTest(JsonSink& sink) { return buildStatePatch(sink); }
    void baselineLeafHashesForTest() { baselineLeafHashes(); }
    void requestFullResyncForTest() { requestFullResync(); }
    bool fullResyncPendingForTest() const { return fullResyncPending_; }
    void clearFullResyncForTest() { fullResyncPending_ = false; }
    /// Install the schema-changed hook WITHOUT opening the TCP listener (setup() does both). A unit
    /// test proving the hook fires the resync needs no socket, and binding a port under test is flaky
    /// (a busy port fails the open). release() unwires it the same way as after a real setup().
    /// The port the live server actually serves on (bound at open, 0 when none is up), NOT the
    /// mutable `port` control: the one true source for any module that must print its own URL
    /// (HlsDriver's `url` control).
    static uint16_t servedPort() { return instance_ ? instance_->boundPort_ : 0; }

    void installSchemaHookForTest() {
        instance_ = this;
        MoonModule::setSchemaChangedHook(&HttpServerModule::onSchemaChanged);
    }

private:
    platform::TcpServer server_;
    Scheduler* scheduler_ = nullptr;
    const char* uiPath_ = "src/ui";

    // Sized for a few concurrent viewers PLUS the transient overlap when one refreshes: the browser opens
    // the new WS before the OS delivers the old socket's FIN, so both briefly hold a slot (a dead one is
    // reaped within a tick or two on its next failed send/poll). At 4 a couple of quick refreshes could
    // fill every slot with not-yet-reaped connections and the new upgrade was rejected ("WebSocket closed
    // before the connection is established"); 8 leaves headroom so a refresh always lands a slot. Each slot
    // is just an fd + a small cursor, so the array stays tiny.
    static constexpr int MAX_WS_CLIENTS = 8;
    platform::TcpConnection wsClients_[MAX_WS_CLIENTS];

    // `/wsp`, the SECOND channel, for lossy binary streams (the preview). Its own connections, so a
    // 10 KB preview frame can never delay a state push: they are separate TCP connections, which is
    // the standard remedy for the head-of-line blocking one socket carrying both traffic classes
    // produces. `/ws` stays the control plane (JSON state + patches).
    //
    // Its cap is DELIBERATELY lower than MAX_WS_CLIENTS. Both arrays draw on one
    // CONFIG_LWIP_MAX_SOCKETS budget of 16, shared with HTTP, mDNS, Art-Net, MQTT and OTA, a
    // preview socket per WS client would consume the whole budget at the cap and starve the rest.
    // 4 is sized from the observed use: one browser almost always, two often enough that it must
    // just work, more only occasionally, while REST callers (Home Assistant, scripts) never open
    // a preview socket at all. A refused upgrade costs that client only its preview; its /ws
    // control connection is untouched.
    static constexpr int MAX_PREVIEW_CLIENTS = 4;
    platform::TcpConnection previewClients_[MAX_PREVIEW_CLIENTS];

    ClientMessageSink* clientSink_ = nullptr;   // the producer's inbound-message sink (PreviewDriver)

    // Resumable full-frame send (BinaryBroadcaster::sendBufferedFrame). One WS message = a copied
    // header + a pointer into the caller's STABLE body buffer (the PreviewDriver producer buffer),
    // drained a bounded chunk per client per tick20ms via writeSome — so a large frame is delivered
    // over wall-clock ticks without spinning any loop, yet stays ONE atomic WS message to the
    // browser. One in flight at a time (drop-new: a frame offered while one is active is rejected,
    // the in-flight one kept). The caller calls cancelBufferedSend() before freeing/reallocating the
    // body (a geometry rebuild), so a cursor never reads freed memory.
    struct PreviewSend {
        // 24, not 16: a payload over 64 KB takes the 10-byte WS length form, and the preview app
        // headers add up to 11 more. 16 silently refused every uncapped-size frame.
        uint8_t hdr[24] = {};                 // WS + app header, copied (caller's may be a stack local)
        size_t hdrLen = 0;
        const uint8_t* body = nullptr;        // the frame body — see ownsBody for lifetime
        size_t bodyLen = 0;
        size_t sent[MAX_PREVIEW_CLIENTS] = {};  // per-PREVIEW-client cursor over [hdr ++ body]; a slow client lags
        bool active = false;
        // body is BORROWED: PreviewDriver keeps its pixel buffer alive; prepare() cancels before a
        // resize frees it. The state push has its own slot (StateSend) since the channel split -
        // sharing this one routed the full state to /wsp and starved every /ws client of its resync.
    };
    PreviewSend previewSend_;
    // Resumable full-state send to the CONTROL channel (`/ws`): same cursor-per-client shape as
    // PreviewSend, but it drains to wsClients_ and always OWNS its JSON body (built per resync,
    // freed on drain-complete / release). Each WS message a client receives stays atomic: while
    // this is active, the patch and WLED pushes to /ws are skipped so nothing interleaves.
    struct StateSend {
        uint8_t hdr[16] = {};
        size_t hdrLen = 0;
        const uint8_t* body = nullptr;
        size_t bodyLen = 0;
        size_t sent[MAX_WS_CLIENTS] = {};
        bool active = false;
    };
    StateSend stateSend_;
    // Guards the PREVIEW channel's shared state: previewSend_ and the previewClients_ sockets,
    // which have producers on TWO cores once the multicore split engages. Core 1 (the offloaded
    // PreviewDriver's tick) arms frames and directly streams the coordinate table; core 0 touches
    // the same sockets and bookkeeping in drainPreviewSend, the uplink reap and /wsp admission.
    // Without it, a partial-write stream on one core interleaves with the other inside one WS
    // frame (corrupt framing), a close lands under a concurrent write on the same fd, or one side
    // observes a torn previewSend_. The CONTROL channel (wsClients_, stateSend_) is deliberately
    // outside its scope: every /ws writer runs on core 0. try_lock only, never a blocking lock:
    // whichever core loses the race SKIPS its slot (the hot-path rule, CLAUDE.md § Hot path); a
    // lost race costs one preview frame or defers a reap/admission one tick, never a stalled
    // render or encode.
    mutable TryLock wsLock_;
    // Queue a TEXT frame (opcode 0x81) whose body this module OWNS into the STATE slot, the
    // (20 KB) full-state JSON drains in chunks to /ws clients on tick20ms instead of a blocking
    // write on the render tick. Takes ownership of `ownedBody` (freed on drain-complete /
    // release). Returns false (and frees ownedBody) if a state send is already in flight -
    // drop-new, the next second's state is fresher.
    bool startBufferedTextSend(char* ownedBody, size_t bodyLen);
    // Drain one memory-adaptive chunk per client of the in-flight resumable send; mark it done when
    // every live client has the whole frame, freeing an owned body then. Called from tick20ms. No-op
    // when none is active.
    void drainPreviewSend();
    // Same, for the in-flight full-state send to /ws clients. Called from tick20ms.
    void drainStateSend();
    // Largest chunk to push per client per drain tick, derived from free contiguous memory so a
    // tight board takes small bites (bounded tick occupancy) and a roomy board drains fast.
    size_t drainChunkBytes() const;

    // All JSON API responses (/api/state, /api/types, /api/system) and the WS
    // state push stream through a JsonSink — no shared fixed-size buffer.

    // --- Diff-on-the-wire state push -----------------------------------------
    // The periodic WS push sends the FULL state once (on connect / after a structural change), then a
    // PATCH of only the controls whose value changed — because a full re-serialize of the whole tree
    // (~34 KB, mostly unchanging option/detail metadata) every second on the render thread is the 1 Hz
    // LED stutter (a periodic tick shares the render thread — see CLAUDE.md "sub-hot path"). Change is
    // detected by value-compare, not a dirty flag: buildStatePatch serializes each control's VALUE,
    // hashes it, and compares to a cached hash — so a value changed by the device itself (telemetry,
    // status, a driver) is caught the same as a setControl write, with no per-write instrumentation.
    // The cache is a flat parallel array (path-hash → value-hash), 8 bytes per control; a global
    // (not per-client) baseline, reset by requestFullResync() on any connect or structural change.
    struct LeafHash { uint32_t path = 0; uint32_t value = 0; };
    // A growable heap array (no fixed MAX — CLAUDE.md "nothing is fixed"): sized to the exact leaf
    // count on each full-state baseline, so a small tree costs ~1 KB and it grows with the tree, never
    // silently dropping a leaf from the cache. The resize is off the hot path (baseline runs on resync,
    // not per tick); the per-tick patch build only reads it. 8 bytes/leaf (path-hash + value-hash).
    ScratchBuffer<LeafHash> leafHashes_{*this};
    uint16_t leafHashCount_ = 0;
    bool fullResyncPending_ = true;    // send a full state next push (set on connect / structural change)
    // Build a patch of changed control values into `sink` as {"patch":[{"path":"<mod>/<ctrl>","value":V},…]};
    // returns the number of changed leaves (0 → nothing to send). Walks the tree, value-hashes each
    // control, updates the cache. Emits a control ONLY when its value-hash differs from the cache.
    uint16_t buildStatePatch(JsonSink& sink);
    // Recompute + store every control's value-hash WITHOUT emitting (baseline the cache to "just sent a
    // full state"), so the next buildStatePatch reports only changes since the full state.
    void baselineLeafHashes();
    // Visit every UI leaf (per-module live telemetry + each control's value) in buildStateJson order,
    // calling fn(pathHash, valueHash, path, valueSink). Templated so the lambda inlines; defined in the
    // .cpp (only used there). findLeaf looks a cached leaf up by path-hash (linear over the flat cache).
    template <class Fn> void forEachStateLeaf(Fn&& fn);
    template <class Fn> void visitModuleLeaves(MoonModule* mod, Fn&& fn);
    LeafHash* findLeaf(uint32_t pathHash);
    // Mark that the next push must be a full state + fresh baseline (a client connected, or the tree
    // structure changed so a value patch can't describe it).
    void requestFullResync() { fullResyncPending_ = true; }

    // Static sink for MoonModule::setSchemaChangedHook: routes any module's rebuildControls() (a
    // schema change) to the live instance's requestFullResync(). instance_ mirrors the
    // FilesystemModule::noteDirty singleton pattern — set in setup(), cleared in release().
    static void onSchemaChanged();
    static inline HttpServerModule* instance_ = nullptr;
    uint16_t boundPort_ = 0;   // the port open() actually bound; 0 when no server is live

    // XOR key for Password-control obfuscation in /api/state. NOT a secret — the
    // same value lives in src/ui/app.js (PW_XOR_KEY). This only stops the
    // password being plainly readable in a raw API response; it is trivially
    // reversible by design (see the ControlType::Password serialization).
    static constexpr uint8_t PASSWORD_XOR_KEY = 0x5A;

    // -----------------------------------------------------------------------
    // HTTP handling
    // -----------------------------------------------------------------------
    void handleConnection(platform::TcpConnection& conn);
    void sendResponse(platform::TcpConnection& conn, int status, const char* contentType, const char* body);
    void sendPreflightResponse(platform::TcpConnection& conn);
    void serveFile(platform::TcpConnection& conn, const char* filename, const char* contentType);

    // File Manager: read/write an arbitrary filesystem path (the /api/file endpoints). `query` is
    // the request's query string, read for `path=<rel>`; the path is vetted (no traversal, rooted
    // at the mount) and size-capped. A file body isn't a control value, so these are their own
    // endpoints rather than /api/control.
    void serveFileContents(platform::TcpConnection& conn, const char* query);
    /// The one streamed-file sender both file routes share: fs path, MIME, extra header lines.
    void streamFsFile(platform::TcpConnection& conn, const char* path, const char* mime,
                      const char* extraHeaders);
    /// One HLS artifact (playlist / segment) from /.hls/, video MIME + no-cache; flat names only.
    void serveHlsFile(platform::TcpConnection& conn, const char* name);
    // Streamed atomic upload: `initialBody`/`initialLen` are the body bytes already in the request
    // buffer; `contentLen` is the declared total. Pulls any remainder off the socket → fsWriteStream,
    // so an upload of any size streams to the file (rejected if it exceeds kUploadMax or free space).
    void handleWriteFile(platform::TcpConnection& conn, const char* query,
                         const char* initialBody, size_t initialLen, size_t contentLen);
    // File Manager: one directory's children as JSON (the /api/dir endpoint) — the source the lazy
    // tree loads a node's children from. Single-level; `hidden=1` in the query includes dotfiles.
    void serveDirListing(platform::TcpConnection& conn, const char* query);
    void handleMakeDir(platform::TcpConnection& conn, const char* query);      // POST /api/dir?path=
    void handleRemoveEntry(platform::TcpConnection& conn, const char* query);  // DELETE /api/dir?path=

    // -----------------------------------------------------------------------
    // JSON state
    // -----------------------------------------------------------------------
    void serveState(platform::TcpConnection& conn);
    void buildStateJson(JsonSink& sink);
    void writeModuleJson(JsonSink& sink, MoonModule* mod);
    void writeControls(JsonSink& sink, MoonModule* mod);
    // Emit `,"status":"…","severity":"…"` for a module that has a status set;
    // no-op when status is null. Shared by writeModuleJson (/api/state) and
    // writeModuleMetricsJson (/api/system) so the two endpoints stay in sync.
    static void writeStatus(JsonSink& sink, MoonModule* mod);

    // -----------------------------------------------------------------------
    // Control setter
    // -----------------------------------------------------------------------
    void handleSetControl(platform::TcpConnection& conn, const char* body);

    // Find a module anywhere in the scheduler's tree by its name — null-guards scheduler_
    // then delegates to Scheduler::firstByName (the one canonical tree-walk).
    MoonModule* findModuleByName(const char* name);

    // -----------------------------------------------------------------------
    // System metrics
    // -----------------------------------------------------------------------
    void serveSystem(platform::TcpConnection& conn);
    /// WLED-compatibility shim — see the class comment + the /json/info route. /json/info
    /// lists the device; /json/state + /json/si carry on/brightness/color for the card;
    /// POST /json/state maps the app's toggle + slider onto Drivers brightness.
    void serveWledInfo(platform::TcpConnection& conn);
    void serveWledState(platform::TcpConnection& conn);
    void serveWledStateInfo(platform::TcpConnection& conn);
    void serveWledDeviceJson(platform::TcpConnection& conn);   ///< /json — HA WLED integration surface
    void serveWledPresets(platform::TcpConnection& conn);      ///< /presets.json — look presets for HA
    void handleWledState(platform::TcpConnection& conn, const char* body);
    void pollWledStateFromWebSockets();             ///< read app's slider/toggle sent over /ws
    void writeWledInfoBody(JsonSink& sink, const char* name, const uint8_t mac[6]);
    void writeWledName(JsonSink& sink, const char* name);   // 💫-prefixed WLED name (HA marker)
    void writeWledStateBody(JsonSink& sink);
    /// Resolve device identity for the WLED shim: `deviceName` (from SystemModule) → `name`,
    /// live IPv4 (Ethernet first, WiFi fallback) → `ip`, MAC → `mac`. Extracted so the
    /// /json/info, /json/si and /json handlers share one lookup instead of hand-copying the
    /// same four platform calls. `nameFallback` defaults to `"projectMM"` (the value the
    /// handlers used before extraction). Both `ip` and `mac` are written in place; `name`
    /// points at the SystemModule string when present (which outlives the request).
    void resolveWledIdentity(const char*& name, uint8_t mac[6], uint8_t ip[4],
                             const char* nameFallback = "projectMM");
    void writeModuleMetricsJson(JsonSink& sink, MoonModule* mod, bool& first);

    // -----------------------------------------------------------------------
    // Module CRUD
    // -----------------------------------------------------------------------
    void handleAddModule(platform::TcpConnection& conn, const char* body);
    void handleDeleteModule(platform::TcpConnection& conn, const char* moduleName);
    void handleReplaceModule(platform::TcpConnection& conn, const char* moduleName, const char* body);
    void serveTypes(platform::TcpConnection& conn);
public:
    /// Delete `path`, and everything under it when it is a directory. Public so the File Manager's
    /// tests exercise the real recursion rather than a copy of it; the HTTP layer is what a user
    /// reaches it through. `depth` bounds the walk (see the definition).
    static bool removeRecursive(const char* path, uint8_t depth = 0);
private:
    // GET /api/scripts → the MoonLive script catalog: which factory scripts exist, per role, plus
    // the repo tag to fetch them from. The UI needs it to offer a script the device does not hold
    // yet; the catalog is compiled in, so this costs no filesystem access.
    void serveScriptCatalog(platform::TcpConnection& conn);

    /// GET /api/modules/<name> — one module's JSON, byte-identical to its entry in /api/state
    /// (children included). `name` is the raw path segment and may be percent-encoded, since a
    /// module name can carry a space ("File Manager").
    ///
    /// For issue reports: the UI puts an `api` link on every card, so a user can copy the state
    /// of the one module that misbehaves rather than the whole tree.
    void serveModule(platform::TcpConnection& conn, const char* name);
    void writeTypeDefaults(JsonSink& sink, const char* typeName);
    void handleMoveModule(platform::TcpConnection& conn, const char* moduleName, const char* body);
    // Editable list (the CRUD primitive): `<tail>` is the path after "/api/list/", i.e.
    // "<module>/<control>[/<id>]". Add appends a row (POST), patch edits/reorders one (PATCH),
    // delete removes one (DELETE). resolveEditableList does the shared parse + validation.
    void handleListAddRow(platform::TcpConnection& conn, const char* tail);
    void handleListPatchRow(platform::TcpConnection& conn, const char* tail, const char* jsonBody);
    void handleListDeleteRow(platform::TcpConnection& conn, const char* tail);
    ListSource* resolveEditableList(platform::TcpConnection& conn, const char* tail,
                                    uint32_t& outId, bool& outHasId);
    MoonModule* listMutationModule_ = nullptr;  // module whose list a CRUD op resolved to (for markDirty)
    void afterListMutation();
    void handleReboot(platform::TcpConnection& conn);
    void handleBootMoonBase(platform::TcpConnection& conn);
    /// OTA: `POST /api/firmware/url` body=`{"url":"..."}`. On a MoonBase device the URL is
    /// staged in NVS and the device reboots into MoonBase, which installs it unattended
    /// (202 + {"moonbase":true}). Otherwise the URL goes to platform::http_fetch_to_ota,
    /// which spawns a task and returns: 202 immediately, progress via FirmwareUpdateModule.
    void handleFirmwareUrl(platform::TcpConnection& conn, const char* body);
    void handleFirmwareUpload(platform::TcpConnection& conn, const char* initialBody,
                              size_t initialLen, size_t contentLen);   // POST /api/firmware/upload
    /// Install a new MoonBase into the factory slot: the mirror of MoonBase installing the app,
    /// and the only way to fix a broken recovery image without a cable. Vets the image before
    /// erasing anything, and does NOT reboot: the running app is untouched.
    void handleMoonBaseUpload(platform::TcpConnection& conn, const char* initialBody,
                              size_t initialLen, size_t contentLen);   // POST /api/firmware/moonbase-update
    /// The same install, fetched by the device from a URL: what makes a GitHub release asset
    /// installable without the browser relaying ~750 KB. Answers 202 and installs on its own
    /// task, so the browser can poll for progress; nothing reboots, so there is no handover.
    void handleMoonBaseUrl(platform::TcpConnection& conn, const char* body);   // POST /api/firmware/moonbase-update-url

    // -----------------------------------------------------------------------
    // WebSocket
    // -----------------------------------------------------------------------
    /// `previewChannel` = the request arrived on `/wsp`, so the connection joins previewClients_
    /// (the lossy binary channel) instead of the control plane's wsClients_.
    void handleWebSocketUpgrade(platform::TcpConnection& conn, const char* req,
                                bool previewChannel = false);
    void pushStateToWebSockets();
    void pushWledStateToWebSockets();   // WLED-app {state,info} frame on /ws (see impl)
    static bool sendWsTextFrame(platform::TcpConnection& conn, const char* data, int len);
};

} // namespace mm
