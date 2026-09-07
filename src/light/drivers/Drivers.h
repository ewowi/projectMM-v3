#pragma once

#include "core/PinList.h"        // parsePinList: the relay list, same parser the LED drivers use
#include "light/drivers/DriverBase.h"  // DriverBase — the Drivers container casts its children to it
#include "core/MoonModule.h"
#include "core/ActiveInstance.h"  // the summary-seat election (the seat + its RAII vacate)
#include "light/layers/Buffer.h"
#include "light/layers/Layer.h"
#include "light/layers/Effects.h"
#include "light/layers/BlendMap.h"
#include "light/drivers/Correction.h"
#include "light/Palette.h"   // the global active palette + its select control
#include "light/moonlive/MoonLivePalette.h"   // a palette computed per frame by a script
#include "light/moonlive/script_catalog.h"       // the tags each factory palette declares
#include "core/LightSummary.h"   // the POD published for the domain-neutral WLED/MQTT consumers
#include "platform/platform.h"

#include <cstring>  // std::strcmp in onControlChanged
#include <atomic>   // encodeDone_ — the render↔encode cross-core handoff flag

namespace mm {

/// Top-level container for one or more drivers — the consumer side of the pipeline.
/// Owns the shared output buffer (when memory allows) and performs blend+map from
/// every layer's buffer into it each frame.
///
/// **Naming convention.** Capital `Drivers` is the container class; lowercase
/// "driver"/"drivers" is the English singular/plural for individual `DriverBase`
/// children. Capitalisation disambiguates "the Drivers container" from "two drivers
/// running" (same rule for `Layouts`/layout and `Effects`/effect).
///
/// **Shared output buffer.** Necessary because blend+map writes to arbitrary physical
/// positions via LUT — the output is not filled sequentially, so a driver cannot read
/// chunk-by-chunk until the full buffer is populated. It uses the same `Buffer` type a
/// Layer does, sized by the Layouts container. Exception: when exactly one layer is
/// enabled AND its mapping is 1:1 unshuffled (no LUT — grid layout, no serpentine),
/// Drivers skips its own buffer and lets drivers read directly from the layer's buffer
/// (the zero-copy fast path, at the cost of parallelism).
///
/// **Multi-layer composition.** When two or more layers are enabled, Drivers composites
/// them into the shared output buffer each frame in Effects container order (bottom→top,
/// via `forEachEnabledLayer`). The bottom layer clears and overwrites the buffer; each
/// layer above blends onto the accumulated frame per its own `blendMode` and `opacity`
/// (the inert per-Layer controls). Drivers owns the orchestration because only it sees
/// the stack order and the output buffer; the layers carry only the parameters. The
/// per-pixel blend math lives in `blendMap` (integer-only, per the hot-path rule). A
/// full-opacity overwrite/additive layer pays no alpha arithmetic, so per-frame cost
/// scales with the enabled-layer count. With a single enabled layer there is no
/// composite: the fast path applies (no-LUT → zero-copy; with a LUT → one blend+map pass
/// into the output buffer).
///
/// **Output correction.** Each *physical* driver owns its own `Correction` (channel-order
/// table, output channel count, white synthesis, and a brightness LUT), applied per-light as
/// it reads the source buffer; Preview ignores it (shows the raw logical buffer). Drivers owns
/// only the GLOBAL `brightness` (plus `on` and `palette`); on a power/brightness change it
/// calls each child's `rebuildCorrection(globalBrightness)`, which bakes global × that driver's
/// local brightness into its LUT. So outputs on one board can differ — a GRB strip and an RGBW
/// panel, each at its own brightness — while every driver still sees the same composited RGB
/// source. Palette model + names follow FastLED's, credited as prior art; implementation in
/// `src/light/Palette.h`.
///
/// **Per-driver source window (`start` / `count`).** A window-aware output driver reads
/// the shared source buffer and outputs a contiguous slice of it — its *window* — making
/// light distribution explicit and order-independent: each driver names its own slice, so
/// reordering drivers does not change which lights each outputs (only tick order). The
/// motivating case: an onboard status LED with window `[0, 1)` and a main strip with
/// window `[1, …)` as two driver instances on the same buffer, neither stealing the
/// other's lights. `DriverBase::addWindowControls()` opts a driver in (see there); a driver
/// that outputs the whole buffer (such as PreviewDriver) simply doesn't call it.
///
/// **Prior art:** MoonLight's PhysicalLayer — owns `channelsD` (display buffer),
/// `compositeLayers()` maps virtualChannels → channelsD, parallelism via a semaphore
/// (driver signals completion, compositor writes)
/// (https://github.com/ewowi/MoonLight/blob/main/src/MoonLight/Layers/PhysicalLayer.h).
/// @card Drivers.png
class Drivers : public MoonModule {
public:
    // Accepts output drivers only, so the "+ add" picker under Drivers offers just the drivers — not
    // every generic system module (Devices, Filesystem, …), which would bury them. The one non-driver
    // child, the boot-wired LightPresetsModule (ModuleRole::Generic), is added directly at boot via
    // addChild — that path bypasses acceptsChildRoles — and is userEditable(false), a non-deletable
    // singleton, so it never needs to be user-added and `generic` isn't needed in this accept list.
    const char* acceptsChildRoles() const override { return "driver"; }

    /// The live light-pipeline summary (light count, channels), for the domain-neutral core
    /// consumers (WLED /json shim, MQTT). Static so a factory-created consumer reaches it without
    /// a wiring inject — the same shape as `AudioService::latestFrame()`. Points at the active
    /// Drivers' summary (set in prepare, vacated in release); a default all-zero summary
    /// when no Drivers is in the tree, so a consumer always reads a valid POD, never null.
    static const LightSummary* latestSummary() {
        static const LightSummary kNone{};
        Drivers* a = ActiveInstance<Drivers>::active();
        return a ? &a->summary_ : &kNone;
    }

    // Vacate the summary seat when this Drivers is removed, so latestSummary() falls back to the
    // all-zero default rather than a dangling pointer (the robustness rule). MoonModule::release
    // recurses to children.
    void release() override {
        // Stop + join the core-1 encode task BEFORE the children release: the worker calls into the
        // driver children's tick(), so it must be quiesced before their buffers/handles free.
        stopEncodeTask();
        renderSplitActive_ = false;
        seat_.vacate();
        // Detach the scripted-palette seam for the same reason the summary seat is vacated: it
        // REFERENCES this object's liveNames_/livePtrs_/liveTags_ arrays rather than copying them,
        // so leaving it published points /api/state at freed memory the moment this Drivers goes.
        LivePalettes::clear(livePtrs_);
        // The scripted-palette instance seam is the same shape and needs the same care: it points at
        // paletteScriptModule_ (a member), and Effects::tick runs it every frame. Detach it and free
        // the compiled script, or the render path keeps executing code owned by a dead Drivers.
        MoonLivePalette::clearActiveInstance(&paletteScriptModule_);
        paletteScriptModule_.release();
        MoonModule::release();
    }

    /// Stop the core-1 task on destruction too, not only on release(). The worker holds a `this`
    /// pointer and calls into the driver children, so a Drivers destroyed without an explicit
    /// release() (a stack instance in a test, a tree torn down by its owner) would leave a thread
    /// dereferencing freed memory. Same dangling-static guard the summary seat uses — a destructor
    /// is the only place that can't be skipped.
    /// The palette seam is cleared here TOO, not only in release(): a destructor is the one place
    /// that cannot be skipped, and a Drivers torn down without an explicit release() would leave
    /// the seam pointing into freed member arrays.
    ~Drivers() override {
        stopEncodeTask();
        LivePalettes::clear(livePtrs_);
        MoonLivePalette::clearActiveInstance(&paletteScriptModule_);
        // Free the compiled script too, not just detach the seam: release() may never run (a stack
        // instance in a test, a tree torn down by its owner) and the engine holds an exec block.
        // release() is idempotent, so the ordinary path that calls both frees once.
        paletteScriptModule_.release();
    }

    /// Stop the core-1 encode worker so a STRUCTURAL TREE MUTATION (a module replace / delete / add) can
    /// free tree nodes without the worker dereferencing them mid-tick. The worker ticks the driver
    /// children, and a driver walks the whole Layouts/Layer tree (PreviewDriver::sendFrame →
    /// Layouts::placeLights), so freeing ANY layout/layer/driver node while core 1 runs is a
    /// use-after-free — a LoadProhibited fault (e.g. replacing a layout on a running split device). The
    /// mutation path (HttpServerModule) calls this before the free; the trailing prepareTree() re-engages
    /// the split. Idempotent + safe when the split is off (stopEncodeTask guards on the task handle).
    void quiesceRenderSplit() {
        stopEncodeTask();
        renderSplitActive_ = false;
    }
    /// Reach the live Drivers (the one that owns the encode worker) to quiesce it around a mutation.
    static Drivers* active() { return ActiveInstance<Drivers>::active(); }

    /// Where this rig's fixtures keep their motion channels, as LAYER slots.
    ///
    /// Callable BEFORE Drivers has prepared, which is the point: a Layer allocates its buffer in
    /// its own prepare, and modules prepare in registration order with Effects ahead of Drivers.
    /// Without this the layer would size itself for color only, an effect's setPan() would fall
    /// outside the light, and a fixture would not move until some later rebuild widened it.
    /// Each driver resolves its preset on demand (rebuildCorrection is idempotent and cold-path).
    /// Hand the fixture layout to every Layer that could render into this rig, so an effect's
    /// setPan() lands on the right byte and the layer can size its light to hold it.
    void publishFixtureChannels() {
        const FixtureChannels fc = fixtureChannels();
        if (effects_) {
            for (uint8_t i = 0; i < effects_->childCount(); i++)
                if (effects_->child(i)->role() == ModuleRole::Layer)
                    static_cast<Layer*>(effects_->child(i))->setFixtureChannels(fc);
        } else if (layer_) {
            layer_->setFixtureChannels(fc);
        }
    }

    FixtureChannels fixtureChannels() {
        FixtureChannels fc;   // every offset absent: a rig with no motion, the common case
        for (uint8_t i = 0; i < childCount(); i++) {
            if (child(i)->role() != ModuleRole::Driver || !child(i)->enabled()) continue;
            auto* d = static_cast<DriverBase*>(child(i));
            d->rebuildCorrection(brightness);        // resolve the preset if it has not been yet
            const Correction& c = d->correction();
            if (!c.hasMotion) continue;
            // Layer slots, not the fixture's channel numbers: packed after RGBW in a fixed order,
            // so an effect's pan write can never collide with the color bytes. forEachMotionSlot
            // is the one definition of that order; Correction::apply reads it back.
            const bool present[5] = {c.offPan    != Correction::kAbsent,
                                     c.offTilt   != Correction::kAbsent,
                                     c.offZoom   != Correction::kAbsent,
                                     c.offRotate != Correction::kAbsent,
                                     c.offGobo   != Correction::kAbsent};
            uint8_t* const dst[5] = {&fc.pan, &fc.tilt, &fc.zoom, &fc.rotate, &fc.gobo};
            FixtureChannels::forEachMotionSlot(present,
                [&](uint8_t role, uint8_t slot) { *dst[role] = slot; });
            break;   // a chain is homogeneous (architecture.md), so one layout describes it
        }
        return fc;
    }

    /// Global brightness (0–255). Scales every channel through a 256-entry LUT
    /// (`(v × brightness) / 255`); changing it rebuilds only the LUT on the cheap
    /// `onControlChanged` tier — no pipeline realloc, so the slider is fluent. Gamma /
    /// white-balance fold into this LUT later as a per-channel R/G/B split.
    ///
    /// Default low (≈8%). A fresh device with LEDs wired but no power budget set
    /// (such as a strip on USB 5V) draws far less at 20 than at full white, so the
    /// first boot can't brown out the board before the user sets a safe level.
    /// The user raises it via the brightness control once their supply is known.
    uint8_t brightness = 20;
    /// Master power. `on=false` outputs black without touching `brightness`, so toggling back
    /// to `on=true` restores the exact level. Implemented by scaling the correction LUT to zero
    /// (effectiveBrightness()) — the same cold-path rebuild brightness uses, no hot-path branch.
    /// This is the single power control every consumer drives: the UI toggle, IR's on/off action,
    /// the WLED app / Home Assistant (`{"on":…}`), and MQTT/Homebridge all set THIS control through
    /// `Scheduler::setControl` — define-once, reuse everywhere (the first slice of a global
    /// lights-control surface). Default on so a freshly-flashed board lights up.
    bool on = true;

    /// The GPIOs that switch the LED power supply, comma-separated, empty where nothing is wired
    /// (which is most boards).
    ///
    /// A relay-gated board cuts the strip's power rather than only its data: the QuinLED Dig-2-Go's
    /// GPIO 12 is its "LED Relay enable pin" (vendor pinout), and with it open a perfectly correct
    /// data line lights nothing.
    ///
    /// A LIST, because boards do not agree on the count and it does not track the LED pins: the
    /// Dig-2-Go has one relay for one output, while the Dig-Next-2 has FOUR relays for TWO outputs.
    /// There is no per-driver mapping to be made, and MoonLight's own model says why: every one of
    /// them carries the same `Relay_LightsOn` role, so they all follow master power together. That
    /// is also why this lives beside `on` rather than on a driver: a relay gates the SUPPLY, which
    /// several drivers share, where a driver's own controls (lightPreset, pins) describe that one
    /// driver's output.
    char relayPins[24] = "";

    /// Multicore render↔encode split (Step 2a): run the drivers' encode+transmit on the
    /// second core while the render loop draws the next frame on core 0, so a frame costs
    /// `max(render, encode)` instead of `render + encode`. The encode is the dominant CPU cost at
    /// scale (~3 µs/light — 50 ms at 16K lights), so moving it off the render core both lifts fps and
    /// stops the encode starving the network stack, which also lives on core 0 (a 19 ms inline encode
    /// dropped the Ethernet link — the measured contention that motivated this).
    ///
    /// It lives HERE, not on a driver: this container owns the whole mechanism — the cross-core handoff
    /// buffer (`outputBuffer_`), the core-1 task, and the frame boundary. There is one split, not one
    /// per driver, and no per-driver opt-out: when it is on, the WHOLE output stage moves — the LED
    /// encode (the dominant CPU cost) and the network/preview frame building alike. A driver that writes
    /// a socket still lands in lwIP, which is pinned to core 0 — so the CPU half offloads while the send
    ///
    /// **ON is simply the better configuration — the switch exists to A/B it (and as an escape hatch),
    /// not because some setups should run it OFF.** OFF forces every driver's tick() back inline on
    /// core 0 — the proven single-core path, byte-for-byte. The split also declines to engage on its
    /// own when no driver exists or
    /// the handoff buffer won't fit (memory-tight board): allocate-and-degrade, never a crash. Toggling
    /// applies live (it re-runs prepare(), which quiesces core 1 before it reallocates and spawns or
    /// stops the task) — no reboot. Sibling of the driver's `doubleBuffer`: that hides the WIRE wait
    /// behind DMA on one core; this hides the ENCODE behind the render on the other core. They stack.
    bool multicore = true;
    // The physical wire format (channel order, RGBW white, per-driver brightness) is owned
    // per driver on DriverBase (defineCorrectionControls) — a GRB strip and an RGBW panel on
    // the same board each carry their own preset. The container owns only the GLOBAL brightness
    // above, which each driver's LUT multiplies with its local brightness.
    /// The global active color palette (index into `mm::palettes::kBuiltins`;
    /// `Rainbow`, `Party`, `Lava`, `Ocean`, …). Palette-driven effects read it via
    /// `Palettes::active()` and color their pixels through `colorFromPalette(index)`, so
    /// changing this recolors every such effect live. The select index expands the chosen
    /// gradient into the active 16-entry palette on `onControlChanged` (cheap, off the hot path).
    uint8_t palette = 0;

    /// Discover the `.mlp` files this device carries and publish their names to the picker.
    ///
    /// Cold path, on defineControls: the list changes when a user saves or deletes a script, which
    /// is exactly when the controls are rebuilt anyway. The names are kept in a member array because
    /// the seam holds POINTERS, so a local would dangle the moment this returned.
    void refreshLivePalettes() {
        liveCount_ = 0;
        // BOTH directories, user first, exactly as resolveScript() loads them: a `.mlp` the UI
        // downloaded sits in the FACTORY directory, and scanning only the user one made every
        // downloaded palette invisible. The picker then said "reopen the picker to select it" and
        // reopening changed nothing, because the file was never going to be listed.
        //
        // User first matters for the dedupe below: editing a factory palette writes a second file
        // of the same name to kScriptDir, and the two must appear as ONE entry, the user's.
        const auto scan = [](const char* dir, void* ctx) {
            platform::fsList(dir, [](const char* name, bool isDir, uint32_t, void* c) {
                auto* self = static_cast<Drivers*>(c);
                if (isDir || self->liveCount_ >= LivePalettes::kMax) return;
                const size_t n = std::strlen(name);
                if (n < 5 || std::strcmp(name + n - 4, moonlive::kPaletteExt) != 0) return;
                // Already seen means the user directory carried it, and that copy SHADOWS this one
                // (resolveScript prefers it), so the factory pass must not add a second row.
                for (uint8_t i = 0; i < self->liveCount_; i++)
                    if (std::strcmp(self->livePtrs_[i], name) == 0) return;
                // A bounded copy rather than snprintf("%s"): a name longer than the slot is TRUNCATED
                // to fit, which is what a picker entry wants, and GCC cannot prove the %s form fits.
                char* slot = self->liveNames_[self->liveCount_];
                const size_t cap = sizeof(self->liveNames_[0]) - 1;
                const size_t copy = n < cap ? n : cap;
                std::memcpy(slot, name, copy);
                slot[copy] = '\0';
                self->livePtrs_[self->liveCount_] = self->liveNames_[self->liveCount_];
                self->liveCount_++;
            }, ctx);
        };
        scan(moonlive::kScriptDir, this);
        scan(moonlive::kFactoryScriptDir, this);
        // Alphabetical, because the picker MERGES this list with the built-ins by name and that
        // merge walks both in order. An unsorted list here would interleave wrongly.
        for (uint8_t i = 1; i < liveCount_; i++)
            for (uint8_t j = i; j > 0 && LivePalettes::cmpName(livePtrs_[j], livePtrs_[j - 1]) < 0; j--) {
                const char* tmp = livePtrs_[j]; livePtrs_[j] = livePtrs_[j - 1]; livePtrs_[j - 1] = tmp;
            }
        // The tags each script declared, from the catalog. A `.mlp` the catalog does not know is a
        // script the user wrote: it keeps its place in the list and simply carries no chips, which
        // is why the lookup falls back to an empty string rather than skipping the file.
        for (uint8_t i = 0; i < liveCount_; i++) {
            liveTags_[i] = "";
            for (size_t c = 0; c < moonlive::kPaletteCatalogCount; c++)
                if (std::strcmp(livePtrs_[i], moonlive::kPaletteCatalog[c]) == 0) {
                    liveTags_[i] = moonlive::kPaletteCatalogTags[c];
                    break;
                }
        }
    }

    char        liveNames_[LivePalettes::kMax][moonlive::kMaxScriptName + 1] = {};
    const char* livePtrs_[LivePalettes::kMax] = {};
    const char* liveTags_[LivePalettes::kMax] = {};
    uint8_t     liveCount_ = 0;

    /// The scripted palette: a `.mlp` name, and the binding that runs it. Empty means the built-in
    /// `palette` select above is in charge, which is the default and the common case. The script is
    /// owned HERE because this module owns the palette, and reached for its per-frame run through
    /// MoonLivePalette's static seam, since the layers sample the palette before this module ticks.
    char paletteScript_[moonlive::kMaxScriptName + 1] = {};
    MoonLivePalette paletteScriptModule_;

    // Two ways to wire the source Layer:
    //  - setEffects(Effects*): bind the container; layer_ is re-resolved from
    //    activeLayer() at every prepareTree. This makes the link self-healing —
    //    a Layer cleared and rebuilt via the API (clear_children + add_module)
    //    is picked up on the next prepareTree without re-running main.cpp wiring.
    //  - setLayer(Layer*): pin a specific Layer directly (test rigs that build a
    //    Layer outside an Effects container). Skips re-resolution.
    void setEffects(Effects* layers) {
        effects_ = layers;
        if (effects_) layer_ = effects_->activeLayer();
    }
    void setLayer(Layer* layer) {
        effects_ = nullptr;  // explicit pin overrides container resolution
        layer_ = layer;
    }

    // The brightness actually fed to the LUT: 0 when powered off, else the set brightness. Keeping
    // `on` and `brightness` independent means "off" never clobbers the level the user chose.
    uint8_t effectiveBrightness() const { return on ? brightness : 0; }

    /// How long a powered-off rig keeps tracking before its heads go still, in seconds.
    ///
    /// `on=false` is asked to mean two different things. Between cues it is a BLACKOUT: a desk
    /// drops intensity and leaves the heads following the look, so the show stays on its clock and
    /// the beams are already in the right place when it comes back. Between sets it is a PARK: the
    /// device is done for now, and a rig grinding through a chase nobody can see is noise in a
    /// quiet room. The duration is what separates them, so the timeout decides rather than the user.
    ///
    /// Effects never stop: only the transmission of motion does. Power returns and the rig rejoins
    /// the show where it now is, rather than resuming a cue that has gone stale.
    uint8_t motionHold = 30;
    static constexpr uint8_t kMotionHoldNever = 0;   ///< 0: keep tracking, the desk behavior

    /// Seconds the rig has been off, counted on tick1s. Stops climbing once the hold expires, so a
    /// device left off for a week does not wrap it.
    uint16_t offSeconds_ = 0;
    /// Whether any enabled driver was aimable at the last check, so the control list is rebuilt
    /// on the transition rather than every second. Seeded false and corrected on the first tick:
    /// a rig that starts with a moving head gets its control one second in, which is a second
    /// after the tree is even renderable.
    bool     movableNow_ = false;

    void defineControls() override {
        controls_.addControl("on", on);   // master power — first so it renders at the top of the card
        // The GPIOs that switch the LED supply, for a board that gates power behind a relay: a
        // comma-separated list, because a board can have one per output (a Dig-Quad has four). Empty
        // on everything else, which is most boards: a deviceModel fills it in, the same way it fills
        // in LED pins (architecture.md, deviceModel owns what is wired on the product).
        controls_.addText("relayPins", relayPins, sizeof(relayPins));
        controls_.addControl("brightness", brightness, 0, 255);
        // ONE picker for both kinds. The scripted palettes are published to the seam Palette.h reads
        // and merged into the same alphabetical list, so a `.mlp` is chosen exactly like a built-in
        // rather than through a second control the user has to know about.
        refreshLivePalettes();
        // Sized from THIS instance's scan (liveCount_), not the seam: defineControls also runs
        // before prepare() has published, and a seam-sized control would cap at the built-ins and
        // reject every scripted index, so selecting a live palette silently did nothing.
        controls_.addPalette("palette", palette, mm::paletteOptions,
                             static_cast<uint8_t>(liveCount_ + mm::palettes::kCount));
        // The EDITOR, shown only while a scripted palette is selected: there is nothing to edit in a
        // built-in gradient, and offering the pane would suggest otherwise.
        //
        // It is an EDITOR, not a second selector. `palette` above owns the choice; this names the
        // file that choice resolved to, so the two cannot disagree. An earlier version let this
        // control pick a script too, which meant a user could select `spectrum` here while `palette`
        // still said something else and the card showed two answers to one question.
        controls_.addFilePath("paletteScript", paletteScript_, sizeof(paletteScript_),
                              moonlive::kPalettePick);
        controls_.setHidden(controls_.count() - 1, !LivePalettes::isLive(palette));
        controls_.setReadOnly(controls_.count() - 1, true);   // the selector is `palette`, above
        // And the script's own controls, which only exist while one is running.
        if (LivePalettes::isLive(palette)) paletteScriptModule_.publishControls(controls_);
        // Only where it can DO something: a rig of LED strips has no aim to hold, so the control
        // would be a question about hardware the user does not have. Same add-then-setHidden shape
        // the renderWait field below uses.
        controls_.addControl("motionHold", motionHold, 0, 240);
        controls_.setHidden(controls_.count() - 1, !fixtureChannels().movable());
        controls_.addControl("multicore", multicore);   // render↔encode split on/off (see the member's doc)
        controls_.setAdvanced(controls_.count() - 1);   // a tuning knob, not a user setting
        // Read-only KPI, the multicore sibling of the driver's frameTime: how long core 0 waited at the
        // frame boundary for core 1's encode. ~0 = render and encode overlap perfectly (the split pays
        // off fully). A large value = the effect is far cheaper than the encode, so core 0 idles — the
        // measured signal that a second (ping-pong) handoff buffer would recover that time. Refreshed
        // in tick1s(). Hidden while `multicore` is off: with no split there is no boundary to wait at,
        // so the number is meaningless — the same add-then-setHidden shape the loopback fields use.
        // Expert-only too, since it only reads on the control it reports for.
        controls_.addReadOnly("renderWait", renderWaitStr_, sizeof(renderWaitStr_));
        controls_.setHidden(controls_.count() - 1, !multicore);
        controls_.setAdvanced(controls_.count() - 1);
        MoonModule::defineControls();  // cascade to driver children (each owns its lightPreset/whiteMode)
    }

    // A global power / brightness change re-bakes every driver's LUT (global × that driver's
    // local brightness) — cheap, no pipeline realloc, which keeps the brightness slider fluent
    // (affectsPrepare stays false for Drivers, so handleSetControl skips prepareTree). The
    // per-driver channel order / white / local brightness live on each driver and rebuild there.
    void onControlChanged(const char* controlName) override {
        if (std::strcmp(controlName, "palette") == 0) {
            if (LivePalettes::isLive(palette)) {
                // A scripted palette: name its file and compile. The script then fills the entries
                // every frame, so there is nothing to expand here.
                std::snprintf(paletteScript_, sizeof(paletteScript_), "%s",
                              LivePalettes::nameAt(LivePalettes::sourceIndex(palette)));
                MoonLivePalette::setActiveInstance(&paletteScriptModule_);
                paletteScriptModule_.setScript(paletteScript_);
                paletteScriptModule_.prepare(*this);
            } else {
                // A built-in: detach any script, or it would keep overwriting the gradient every
                // frame and the selection would appear to do nothing.
                MoonLivePalette::setActiveInstance(nullptr);
                paletteScript_[0] = 0;
                Palettes::setActive(LivePalettes::sourceIndex(palette));
            }
            rebuildControls();   // the editor and the script's controls appear or disappear with it
            return;
        }
        if (std::strcmp(controlName, "paletteScript") == 0) {
            // Published to the seam Effects::tick reads. Only while a name is set, so clearing the
            // field detaches the script rather than leaving a compiled one running unseen.
            MoonLivePalette::setActiveInstance(paletteScript_[0] ? &paletteScriptModule_ : nullptr);
            paletteScriptModule_.setScript(paletteScript_);
            paletteScriptModule_.prepare(*this);
            rebuildControls();              // the compile re-derives the script's own controls
            // Clearing the name hands the palette back to the built-in select, which would otherwise
            // keep whatever the script last wrote.
            if (paletteScript_[0] == 0) Palettes::setActive(palette);
            return;
        }
        if (std::strcmp(controlName, "multicore") == 0) {
            // `renderWait` is only meaningful while the split runs, so it's a conditional-hidden control:
            // re-derive the schema so the row appears/disappears with the switch, live (the one
            // rebuildControls chokepoint, which also fires the WS resync). The split itself engages
            // via affectsPrepare → the prepare sweep; this is purely the visible control set.
            rebuildControls();
            return;
        }
        if (std::strcmp(controlName, "on") == 0 ||
            std::strcmp(controlName, "brightness") == 0) {
            rebuildAllCorrections();
        }
        // The relay follows `on` and brightness, and is written HERE rather than per frame:
        // switching power is a state change, not a per-frame concern, and a mechanical relay would
        // wear out doing it at frame rate (the Dig-2-Go's is solid-state, but the rule holds for the
        // ones that are not). Also written when the pin itself changes, so entering it on a live
        // device closes the relay straight away instead of at the next toggle. A brightness drag
        // costs at most one edge, at the zero boundary: the level only changes there.
        if (std::strcmp(controlName, "on") == 0 || std::strcmp(controlName, "brightness") == 0 ||
            std::strcmp(controlName, "relayPins") == 0) {
            applyRelay();
        }
    }

    /// Drive the power relay: closed while `on` and brightness is above zero, open otherwise.
    ///
    /// Some boards gate the LED supply behind a relay (the QuinLED Dig-2-Go's GPIO 12 is its "LED
    /// Relay enable pin"), so the data line can be perfectly correct and the strip stay dark. The
    /// relay belongs to `on` rather than to a driver or a service: it is the physical expression of
    /// master power, and `on` is the one control the UI, the WLED bridge, MQTT and OSC all already
    /// write. A second on/off would be a split brain, with nothing able to say which one meant off.
    /// Brightness 0 opens it too: a strip at zero draws its idle current for nothing, and a slider
    /// at 0 is how a WLED-style client says "off" without touching `on`.
    void applyRelay() {
        const bool closed = on && brightness > 0;
        // Release a pin the user just cleared, or it stays asserted forever on a GPIO nothing owns.
        // The same reasoning InfraredService applies when its pin is unset.
        if (!relayPins[0]) {
            for (uint8_t i = 0; i < lastRelayCount_; i++)
                platform::gpioWrite(static_cast<uint8_t>(lastRelayPins_[i]), false);
            lastRelayCount_ = 0;
            return;
        }
        uint16_t pins[kMaxRelays] = {};
        uint8_t n = 0;
        // parsePinList returns a static error literal, which every other caller feeds straight into
        // setStatus. Reporting it is the difference between a typo'd list and a working one.
        if (const char* err = parsePinList(relayPins, pins, kMaxRelays, n)) {
            setStatus(err, Severity::Warning);
            // Release what the OLD list held before giving up. A typo mid-edit would otherwise leave
            // the previous relays asserted on GPIOs no control names any more, so the strip stays
            // powered after brightness reaches zero and nothing in the UI explains why. An
            // unparseable list means no relays, which is the same state as an empty one.
            for (uint8_t i = 0; i < lastRelayCount_; i++)
                platform::gpioWrite(lastRelayPins_[i], false);
            lastRelayCount_ = 0;
            return;
        }
        // Release the pins that are LEAVING the list before driving the new one. Clearing the list
        // entirely is handled above, but shrinking it is the same problem: editing "12,13" to "12"
        // left 13 asserted forever on a GPIO nothing owns any more, with no control naming it.
        for (uint8_t i = 0; i < lastRelayCount_; i++) {
            bool stillListed = false;
            for (uint8_t j = 0; j < n; j++)
                if (lastRelayPins_[i] == static_cast<uint8_t>(pins[j])) { stillListed = true; break; }
            if (!stillListed) platform::gpioWrite(lastRelayPins_[i], false);
        }
        for (uint8_t i = 0; i < n; i++) {
            // An input-only pin (classic ESP32 34-39) refuses the write, and the seam says so: a
            // relay wired to one would otherwise look configured and do nothing.
            if (!platform::gpioWrite(static_cast<uint8_t>(pins[i]), closed))
                setStatus("relay pin cannot drive an output", Severity::Warning);
            lastRelayPins_[i] = static_cast<uint8_t>(pins[i]);
        }
        lastRelayCount_ = n;
    }

    /// Relays one device can carry. Four is the most any board in the catalog wires (Dig-Next-2);
    /// eight leaves room without costing anything, the list being text.
    static constexpr uint8_t kMaxRelays = 8;

    /// The pins actually driven last time, so clearing the list can release them. The control's text
    /// is gone by then, which is why the numbers are kept rather than re-parsed.
    uint8_t lastRelayPins_[kMaxRelays] = {};
    uint8_t lastRelayCount_ = 0;

    /// `multicore` is the one Drivers control that is STRUCTURAL: it decides whether the cross-core
    /// handoff buffer is allocated and the core-1 encode task runs, both of which live in prepare().
    /// So it alone routes through the prepare sweep (quiescing core 1 before it reallocates), while
    /// on / brightness / palette stay on the cheap correction tier that keeps the sliders fluent.
    bool affectsPrepare(const char* name) const override {
        return std::strcmp(name, "multicore") == 0;
    }

    /// Refresh the read-only `renderWait` KPI once a second (off the hot path, same tier as the driver's
    /// frameTime). Reports the PEAK core-0 wait at the handoff boundary over the last second, not a
    /// single frame's — a lone sample lands wherever tick1s happens to fall and reads ~0 even when the
    /// core is idling most frames. The peak is the number the Step 2b (ping-pong buffer) decision
    /// wants: how much time core 0 gives up at worst. A dash when the split isn't running.
    void tick1s() MM_NONBLOCKING override {
        if (renderSplitActive_) std::snprintf(renderWaitStr_, sizeof(renderWaitStr_), "%u µs",
                                              static_cast<unsigned>(renderWaitPeakUs_));
        else                    std::snprintf(renderWaitStr_, sizeof(renderWaitStr_), "—");
        renderWaitPeakUs_ = 0;   // start a fresh window
        updateMotionHold();
        MoonModule::tick1s();
    }

    /// Count the rig's time powered off, and park it once the hold expires.
    ///
    /// Runs on the 1 Hz tick, which is the resolution this needs: the difference between a cue gap
    /// and a set break is tens of seconds, not milliseconds. Writing the flag straight into each
    /// driver's Correction rather than re-deriving it: the hold changes what is TRANSMITTED, not
    /// what the preset says, so a full rebuildCorrection would be the wrong cost and would fight
    /// the brightness LUT it shares.
    void updateMotionHold() MM_NONBLOCKING {
        // Whether the control is SHOWN follows the rig, and the rig changes when a child driver
        // picks a different light preset. That write rebuilds the child's own controls, never this
        // container's, so without re-deriving here the row stays hidden after a user selects a
        // moving head (and stays visible after they leave one), against the rule that every setting
        // applies live. Compared rather than rebuilt blindly: rebuildControls() fires a WS resync,
        // and this runs every second.
        //
        // Read from each driver's ALREADY-RESOLVED correction rather than through
        // fixtureChannels(), which calls rebuildCorrection() on every child: that re-walks the
        // preset roles and rebuilds a 256-entry brightness LUT per driver, which is exactly the
        // "wrong cost" the rebuildCorrection doc below names, once a second on the render tick.
        bool movable = false;
        for (uint8_t i = 0; i < childCount() && !movable; i++) {
            if (child(i)->role() != ModuleRole::Driver || !child(i)->enabled()) continue;
            const Correction& c = static_cast<DriverBase*>(child(i))->correction();
            movable = c.offPan != Correction::kAbsent || c.offTilt != Correction::kAbsent;
        }
        if (movableNow_ != movable) {
            movableNow_ = movable;
            rebuildControls();
        }
        if (on) {
            offSeconds_ = 0;
        } else if (offSeconds_ < 0xFFFF) {
            offSeconds_++;
        }
        // 0 means never park: keep tracking however long the power is off, which is what a lighting
        // desk does and what a show running to timecode wants.
        const bool held = !on && motionHold != kMotionHoldNever && offSeconds_ >= motionHold;
        for (uint8_t i = 0; i < childCount(); i++) {
            if (child(i)->role() != ModuleRole::Driver) continue;
            static_cast<DriverBase*>(child(i))->setMotionHeld(held);
        }
    }

    /// Re-resolve every driver's correction (preset roles + brightness LUT) into its flat
    /// Correction, WITHOUT re-preparing the tree. This is the correction-only path: a global
    /// brightness change AND a light-preset edit both need it, but neither is a structural
    /// change, so routing them through prepare() would needlessly reinit each driver's output
    /// peripheral (an RMT channel teardown blanks the strip for a tick — Live-reconfiguration
    /// says a config change applies with no visible glitch). rebuildCorrection() calls the
    /// driver's onCorrectionChanged() (resize the correction buffer) but never its prepare(),
    /// so the peripheral is left running. The LightPresetsModule (role Generic) is skipped — only
    /// a Driver owns a correction.
    void rebuildAllCorrections() {
        for (uint8_t i = 0; i < childCount(); i++) {
            if (child(i)->role() != ModuleRole::Driver) continue;
            static_cast<DriverBase*>(child(i))->rebuildCorrection(effectiveBrightness());
        }
    }

    void setup() override {
        Palettes::setActive(palette);   // seed the global active palette from the persisted index
        MoonModule::setup();
        passBufferToDrivers();           // seeds each driver's correction via rebuildCorrection()
        // Tell the layers where this rig's fixtures keep their motion channels, HERE rather than in
        // prepare(): every module's setup() runs before any module's prepare(), and a Layer sizes
        // its buffer in prepare. Pushed later, a layer would size itself for color on a cold boot,
        // an effect's setPan() would fall outside the light, and a fixture would not move until
        // some later rebuild widened it.
        publishFixtureChannels();
        // Close the relay for the persisted `on` at boot. Without this a relay board comes up dark
        // until something toggles the control, which reads as a dead device.
        applyRelay();
    }

    void prepare() override {
        // Discover and PUBLISH the scripted palettes. Publication lives here and not in
        // defineControls() because the seam is one static slot whose last writer wins, and
        // defineControls() also runs on detached instances (the /api/modules probe builds one to
        // read its controls, then destroys it): such a probe would take the seam and empty it on
        // its way out, and every `.mlp` vanished from the running picker. prepare() runs only on a
        // module the scheduler actually mounted, which is exactly the owner the seam wants.
        const uint8_t hadLive = liveCount_;
        refreshLivePalettes();
        LivePalettes::set(livePtrs_, liveTags_, liveCount_);
        // A CHANGED count needs the control rebuilt, not just the seam republished: `palette`'s
        // maximum is baked in defineControls() as liveCount_ + kCount, so a palette downloaded
        // while running was listed by the picker and then REFUSED with "value out of range",
        // because the control still carried the old cap. A write reaches here through
        // applyFileChanged() -> requestPrepareTree(), so this is where the new file first appears.
        if (liveCount_ != hadLive) rebuildControls();
        // Re-resolve the active Layer from the bound container so a Layer that
        // was cleared and rebuilt via the API is picked up here (self-healing).
        // setLayer() pins a Layer directly and leaves effects_ null — skip then.
        if (effects_) layer_ = effects_->activeLayer();
        // The output (composition) buffer is needed when we must blend into a
        // physical-space buffer rather than hand a driver a Layer's logical buffer
        // directly: whenever ≥2 layers composite, OR a single layer has a LUT
        // (logical≠physical). A lone no-LUT layer needs no output buffer (drivers
        // read its buffer directly — the zero-copy fast path).
        // If allocation fails (no contiguous heap — a real risk on no-PSRAM ESP32
        // with fragmented DRAM), outputBuffer_ stays data_=nullptr; tick() checks
        // that before blending (else a null deref panics — same defensive pattern
        // Layer::allocateBuffer uses). Sized from the active layer: every layer
        // composites into the same physical space, so its physicalLightCount() /
        // channelsPerLight() is the composite extent.
        // Output selection keys off an *enabled* source layer, never the disabled
        // fallback activeLayer() may return (which exists only so geometry stays
        // queryable while every layer is toggled off). With no enabled layer there
        // is nothing to emit, so no output buffer — drivers go idle (see
        // passBufferToDrivers). A pinned setLayer() (effects_ null) is always treated
        // as the live source.
        Layer* const out = effects_ ? effects_->firstEnabledLayer() : layer_;
        const uint8_t enabled = effects_ ? effects_->enabledLayerCount() : (layer_ ? 1 : 0);
        const bool needOutput = out && (enabled > 1 || out->lut().hasLUT());

        // The render↔encode split wants an outputBuffer_ EVEN in the identity case (a lone no-LUT
        // layer that would otherwise be zero-copy) — it's the stable frame core 1 reads while core 0
        // renders the next one. So force the buffer when the split is wanted and there are lights to
        // drive. If it can't allocate (low memory) the split simply won't engage below and we fall
        // back to the inline zero-copy path — the memory-tight board pays nothing and never lands in
        // a half-split state (no task is spawned, so there is no cross-core wait to deadlock on).
        // A source with ZERO lights (every layout toggled off) is not a buffer to claim — allocate(0)
        // fails and would print a spurious DEGRADE, so gate on a real light count.
        publishFixtureChannels();

        const bool haveLights = out && out->physicalLightCount() > 0;
        const bool splitWanted = multicore && anyDriver() && haveLights;
        const bool wantOutput = (needOutput && haveLights) || splitWanted;

        // Core 1 may be mid-encode from the current outputBuffer_ — wait it out before free/realloc.
        // If it does NOT come back (a wedged worker), do NOT then free the buffer it may still be
        // reading: tear the task down first. stopEncodeTask() joins, so once it returns nothing can
        // touch outputBuffer_ and the realloc below is safe. This is the cold path, so a blocking join
        // is allowed here — unlike the tick() boundary, which must never block indefinitely.
        if (!quiesceEncode()) stopEncodeTask();
        if (wantOutput) {
            if (!outputBuffer_.allocate(out->physicalLightCount(), out->channelsPerLight())) {
                std::printf("  DEGRADE  Drivers::outputBuffer_ allocate failed for %u lights\n",
                            static_cast<unsigned>(out->physicalLightCount()));
                outputBuffer_.free();   // leaves data_=nullptr, bytes()=0
            }
        } else {
            outputBuffer_.free();
        }
        setDynamicBytes(outputBuffer_.bytes());

        // Engage predicate: split ON iff multicore is on, a driver exists with lights, AND the handoff
        // buffer actually allocated. Decided from the alloc OUTCOME (no if constexpr(hasPsram)) — so a
        // memory-tight board that can't claim the buffer never enters a half-split state: no task is
        // spawned, every driver ticks inline on core 0, and the driver reads the layer buffer
        // (zero-copy). It still keeps doubleBuffer's DMA overlap, which needs no handoff buffer.
        // This is a live-reconfigure — a grid resize, a layer add/delete, or the `multicore` switch
        // flips it, applied here with no reboot:
        //   - newly engaged: spawn the core-1 task.
        //   - newly disengaged (switch off, config reverted, or the buffer no longer fits): stop+join.
        const bool shouldSplit = splitWanted && outputBuffer_.data();
        if (shouldSplit && !renderSplitActive_) {
            renderSplitActive_ = true;
            startEncodeTask();                 // spawns the task (at boot it just parks in waitNotify)
            // The task couldn't be created (startEncodeTask cleared the flag): drop the handoff buffer
            // too. It exists ONLY to be read by core 1 — keeping it would leave passBufferToDrivers()
            // pointing every driver at a buffer nothing composites into, so they'd render the last
            // frame forever. Freeing it re-selects the layer buffer (the inline zero-copy path).
            if (!renderSplitActive_ && !needOutput) {
                outputBuffer_.free();
                setDynamicBytes(outputBuffer_.bytes());
            }
        } else if (!shouldSplit && renderSplitActive_) {
            stopEncodeTask();                  // drains core 1 before we leave split mode
            renderSplitActive_ = false;
            // Turning multicore OFF is a choice, not a fault: a stall warning from the old split
            // must not outlive it (the card showed "encode worker stalled" beside a multicore
            // toggle that was already off).
            if (encodeStalled_) { encodeStalled_ = false; setStatus("", Severity::Status); }
        }
        // Publish the light-pipeline summary for the domain-neutral core consumers (the WLED
        // /json shim, MQTT) via the static latestSummary() pull. `out` is the composite extent;
        // no enabled layer → zero lights. One POD, overwritten in place on each rebuild.
        summary_.lightCount = out ? static_cast<uint32_t>(out->physicalLightCount()) : 0;
        summary_.channelsPerLight = out ? out->channelsPerLight() : 3;
        seat_.claim();   // first live Drivers wins the summary seat (claim-if-empty; one exists in practice)
        passBufferToDrivers();
    }

    // First output light as RGB — the live color of pixel 0, read from whichever buffer
    // tick() is currently driving (the composited outputBuffer_ when allocated, else the
    // first enabled layer's own buffer — the zero-copy single-layer path). The WLED shim
    // tints the app's device card with this. RGB is the buffer's logical channel order
    // (0,1,2); the per-strip wire reorder is applied later by the physical drivers, not here.
    bool firstOutputRgb(uint8_t out[3]) const override {
        const Buffer* src = nullptr;
        if (outputBuffer_.data()) src = &outputBuffer_;
        else if (Layer* l = effects_ ? effects_->firstEnabledLayer() : layer_; l && l->buffer().data())
            src = &l->buffer();
        if (!src || src->count() == 0 || src->channelsPerLight() < 3) return false;
        const uint8_t* p = src->data();
        out[0] = p[0]; out[1] = p[1]; out[2] = p[2];
        return true;
    }

    void tick() MM_NONBLOCKING override {
        // Split active: core 1 is encoding the PREVIOUS frame from outputBuffer_. Wait for it to
        // finish before overwriting the shared buffer (the boundary). The stall is timed — it's the
        // Step 2b trigger metric: ~0 when render ≈ encode (heavy effect), large when render ≪ encode.
        if (renderSplitActive_) {
            uint32_t s0 = platform::micros();
            // A TIMED-OUT quiesce means the worker is wedged — quiesceEncode() cleared renderSplitActive_
            // so future ticks run inline, but THIS tick is about to composite outputBuffer_ and tick every
            // driver inline on core 0 while the wedged worker may STILL be inside a driver's tick() on core
            // 1 (two cores in one driver → double transmit, corrupted inFlight_). So JOIN it first, exactly
            // as prepare() and quiesce() do — the join is slow, but this is the declared-broken path.
            if (!quiesceEncode()) stopEncodeTask();
            renderWaitUs_ = static_cast<uint32_t>(platform::micros() - s0);
            if (renderWaitUs_ > renderWaitPeakUs_) renderWaitPeakUs_ = renderWaitUs_;   // the 1 s window's worst, for the KPI
        }
        // Composite into outputBuffer_ when one is allocated (≥2 enabled layers,
        // or a single layer with a LUT — see prepare). A null data_ means
        // prepare couldn't claim a block (heap fragmentation): skip the blend;
        // drivers then read the raw Layer buffer / send nothing.
        //
        // The single-layer source, resolved ONCE: both single-layer branches below need the same
        // value, and declaring it per-branch in an if-init shadowed the outer one (MSVC C4456 —
        // legitimately: two `Layer* out` in one chain reads as a bug even when it isn't).
        Layer* srcLayer = effects_ ? effects_->firstEnabledLayer() : layer_;

        if (outputBuffer_.data() && effects_ && effects_->enabledLayerCount() > 1) {
            // Multi-layer composite: blend each enabled layer in container order.
            // The first (bottom) layer clears + overwrites; each subsequent layer
            // blends onto the accumulated frame per its own blendMode + opacity.
            // blendMap resolves the op/opacity branch once per layer (a tight
            // specialized loop each — no-LUT layers blend 1:1, LUT layers map),
            // and a full-opacity additive/overwrite layer pays no alpha math, so
            // cost scales with enabled-layer count only.
            effects_->forEachEnabledLayer([&](Layer* L, bool first) {
                BlendOp op = first ? BlendOp::Overwrite : L->blendOp();
                uint8_t op_opacity = first ? 255 : L->opacity;
                blendMap(L->buffer(), outputBuffer_, L->lut(), L->channelsPerLight(),
                         op, op_opacity, /*clearFirst=*/first);
            });
        } else if (outputBuffer_.data() && srcLayer && srcLayer->lut().hasLUT()) {
            // Single layer with a LUT (the only enabled one, or a pinned setLayer):
            // map its logical buffer into physical space. The original fast path.
            // `srcLayer` is the enabled source, never activeLayer()'s disabled fallback;
            // the outputBuffer_.data() guard already excludes the all-disabled case
            // (needOutput is false then), this keeps the source choice explicit.
            blendMap(srcLayer->buffer(), outputBuffer_, srcLayer->lut(), srcLayer->channelsPerLight());
        } else if (renderSplitActive_ && outputBuffer_.data() && srcLayer) {
            // Split active + the identity case (a lone no-LUT layer): normally drivers would read the
            // layer's buffer directly (zero-copy), but core 1 must NOT read a buffer core 0's effects
            // are mutating — so copy the frame into the split-owned outputBuffer_ (a 1:1 map through
            // an identity LUT). This is why prepare() forces outputBuffer_ in split mode even here.
            blendMap(srcLayer->buffer(), outputBuffer_, srcLayer->lut(), srcLayer->channelsPerLight());
        }
        // (Split OFF + a lone no-LUT layer: outputBuffer_ is null, drivers read the logical buffer
        // directly — the zero-copy path set in passBufferToDrivers, unchanged.)
        //
        // Split active: hand the freshly-composited frame to core 1, which runs the WHOLE output stage
        // (every driver's tick). Core 0 returns immediately to render the next frame — it ticks only
        // the non-Driver children (the LightPresetsModule). Note what does NOT move: a driver that
        // writes a socket (NetworkSend's sendto, Preview's WebSocket) still lands in lwIP, which is
        // pinned to core 0 — so the CPU half (packet/frame building) offloads while the send executes
        // on the network stack's own core. That's the intent, not a leak: core 1 becomes the producer,
        // core 0's network task stays the sender.
        // Split off: tick every child inline as before — the proven single-core path.
        if (renderSplitActive_) {
            encodeDone_.store(false, std::memory_order_release);
            platform::notifyTask(encodeTask_);
            tickNonDriverChildren();
        } else {
            MoonModule::tick();   // parent already composited; base ticks all children
        }
    }

    // Core 0 keeps every child that is NOT a Driver while the split runs (today the LightPresetsModule,
    // role Generic); the core-1 worker takes the Drivers. Both sides go through core's one
    // tickChildren gate+timing loop — this container picks the SIDE, it does not re-implement the rule.
    void tickNonDriverChildren() { tickChildren(&MoonModule::tick, RoleFilter::Except, ModuleRole::Driver); }

    // Stop the core-1 worker from reading our child array / a child we're about to free. Core calls
    // this before every structural mutation (addChild / removeChild / replaceChildAt — see
    // MoonModule::quiesce), which is what makes a live driver delete safe while an encode is in flight:
    // without it, core 0 frees the driver's DMA buffers and the object while core 1 is inside its
    // tick(). Waiting out the in-flight encode is sufficient (the worker only runs between a notify and
    // encodeDone_, and the next notify can't come until core 0 returns to tick() — same thread as the
    // mutation), so the task stays alive and the split survives the mutation; prepare() re-evaluates
    // right after and stops the task if the last driver just left.
    //
    // If the worker does NOT come back within the timeout, the caller is about to DELETE the very
    // object it may still be inside — so tear the task down (a join) before returning. Letting a
    // timed-out quiesce return is the use-after-free this whole hook exists to prevent.
    void quiesce() override { if (!quiesceEncode()) stopEncodeTask(); }

private:
    Effects* effects_ = nullptr;  // bound container; layer_ re-resolved from it at prepareTree
    Layer* layer_ = nullptr;
    Buffer outputBuffer_;

    // Published to core via latestSummary(). The seat points at the Drivers whose summary is live —
    // claimed in prepare(), vacated in release() and (via the ActiveInstance destructor) on teardown,
    // so a removed Drivers never leaves latestSummary() dangling. Only one Drivers exists in the
    // pinned tree, so claim-if-empty needs no re-election dance — but the RAII vacate is the same
    // dangling-static guard the mic + registry seats use (see ActiveInstance.h).
    LightSummary summary_;
    ActiveInstance<Drivers> seat_{*this};

    // --- Multicore render↔encode split (Step 2a) ------------------------------------------------
    // When engaged, the OFFLOADABLE driver children (I80/Parlio — pure SWAR encode + DMA) run their
    // tick() on a core-1 task, reading outputBuffer_, while core 0 renders the next frame. The
    // boundary is one shared outputBuffer_: core 0 waits encodeDone_ before overwriting it (the
    // cheap composite is the only serialization; the two heavy stages — render, encode — overlap).
    // Not engaged (multicore off, low memory, no driver, or an identity buffer that won't fit) → every
    // child ticks inline exactly as before. See docs/history/plans/Plan-20260713 - Multicore Step 2.
    platform::WorkerTask encodeTask_{};
    std::atomic<bool> encodeDone_{true};   // core 1 sets true when its encode finishes; core 0 waits it
    std::atomic<bool> encodeStop_{false};  // stop flag the worker fn observes via a woken waitNotify
                                           // (atomic, not volatile: volatile is not a thread primitive
                                           // in C++ — a cross-thread flag is a data race without it)
    bool renderSplitActive_ = false;   // the split is engaged (task spawned, boundary in effect)
    bool encodeStalled_ = false;       // a stall warning is showing, so recovery can clear it
    uint32_t renderWaitUs_ = 0;                 // last frame's core-0 wait at the boundary (the tick-line KPI)
    uint32_t renderWaitPeakUs_ = 0;             // worst wait in the current 1 s window (what the control shows)
    char renderWaitStr_[32] = {};               // the `renderWait` read-only control's text (refreshed in tick1s)

    // Core-1 body: block for a notify, run EVERY driver child's tick() against the finished frame in
    // outputBuffer_, signal done. Reached only while renderSplitActive_. One rule, no per-driver
    // opt-out: when the split is on, the whole output stage lives on core 1 — the LED encode (the
    // dominant CPU cost) and the network send (ArtNet at 16K is the other big one) both leave the
    // render core. Each driver's tick() is unchanged; only which core calls it differs.
    void runEncodeLoop() {
        // Subscribe THIS (core-1) task to the WDT before feeding it: the subscription is per-task, so the
        // taskWdtReset() calls below only count once this task itself is added. Without this, feeding a
        // subscription the render task made on a DIFFERENT task is rejected as "task not found" and floods
        // the log every frame, starving the network stack (the WS drops → reconnect → full-state → UI churn).
        platform::taskWdtSubscribe();
        while (!encodeStop_.load(std::memory_order_acquire)) {
            if (!platform::waitNotify(encodeTask_, 100)) { platform::taskWdtReset(); continue; }
            if (encodeStop_.load(std::memory_order_acquire)) break;
            // Every Driver child, through core's one gate+timing loop (per-driver timing still accrues,
            // now on core 1). encode + transmit / build + send, all reading outputBuffer_.
            tickChildren(&MoonModule::tick, RoleFilter::Only, ModuleRole::Driver);
            platform::taskWdtReset();
            encodeDone_.store(true, std::memory_order_release);
        }
        platform::taskWdtUnsubscribe();   // leave the WDT clean before the task exits (no dangling entry)
    }
    static void encodeTrampoline(void* self) { static_cast<Drivers*>(self)->runEncodeLoop(); }

    // Wait for core 1 to finish the in-flight encode, so core 0 can safely overwrite / free
    // outputBuffer_. Normally bounded by ONE encode (the `renderWait` KPI measures exactly this wait);
    // polled with a yield. No-op when the split is off. The render-side analog of
    // ParallelLedDriver::drainInFlight.
    //
    // The timeout is the robustness floor, not the expected path: a wedged core-1 task, a starved
    // worker, or a lost notify would otherwise spin the RENDER loop forever — and a permanent wedge
    // ranks below "degraded" in the robustness rule (a device must keep running, even poorly). So
    // give up after kQuiesceTimeoutMs and DISENGAGE the split: every driver falls back to ticking
    // inline on core 0, which is the same single-core path a memory-tight board already takes. Slower,
    // still lit. Returns false when it timed out, so a caller that was about to free the handoff buffer
    // knows core 1 might still be reading it and can leave it alone.
    static constexpr uint32_t kQuiesceTimeoutMs = 500;   // ≫ any real encode (~50 ms at 16K lights)
    bool quiesceEncode() {
        if (!renderSplitActive_) return true;
        const uint32_t deadline = platform::millis() + kQuiesceTimeoutMs;
        while (!encodeDone_.load(std::memory_order_acquire)) {
            if (platform::millis() > deadline) {
                setStatus("encode worker stalled - running single-core", Severity::Warning);
                encodeStalled_ = true;        // remember, so recovery can clear the warning
                renderSplitActive_ = false;   // stop notifying it; drivers tick inline from here
                return false;
            }
            platform::yield();
        }
        // The worker answered. Clear a previous stall warning, which is otherwise STICKY: it was
        // set once and never lifted, so a card kept reporting "stalled" long after the split was
        // healthy, and even after multicore was switched off. A status that cannot go away tells
        // the user nothing about now.
        if (encodeStalled_) {
            encodeStalled_ = false;
            setStatus("", Severity::Status);
        }
        return true;
    }

    // Is there an enabled driver child at all? The split only engages when there's output work to move
    // — with no driver there is nothing for core 1 to do, so we don't spawn a task or claim a buffer.
    bool anyDriver() const {
        for (uint8_t i = 0; i < childCount(); i++) {
            MoonModule* c = child(i);
            if (c->role() != ModuleRole::Driver) continue;   // skips LightPresetsModule (Generic)
            if (c->respectsEnabled() && !c->enabled()) continue;
            return true;
        }
        return false;
    }

public:
    /// True while the render↔encode split is engaged (multicore on, a driver exists, AND outputBuffer_
    /// allocated AND the core-1 task is live). Diagnostics / tests read it.
    bool renderSplitActive() const { return renderSplitActive_; }
    /// The WORST core-0 wait at the frame boundary in the current 1 s window (µs) — time given up
    /// waiting for core 1 to finish the output stage. This is the number both the `renderWait` control and
    /// the tick line report: a single frame's value lands wherever the once-a-second sample happens to
    /// fall and reads ~0 even when the core idles most frames, so the peak is the honest signal. It is
    /// the Step 2b (ping-pong 2nd buffer) trigger: ~0 = render ≈ output, a 2nd buffer gains nothing;
    /// large = the effect is far cheaper than the output work, so core 0 idles and 2b would recover it.
    uint32_t renderWaitPeakUs() const { return renderWaitPeakUs_; }
    /// Test-only: the frame-boundary wait in isolation (false = it timed out and disengaged the split).
    /// tick() reaches it inline; a test needs it separately to time the boundary WITHOUT also running
    /// the fallback inline tick that follows a timeout. Same public-for-tests convention as
    /// renderSplitActive() / renderWaitPeakUs().
    bool quiesceEncodeForTest() { return quiesceEncode(); }

private:
    // Spawn the core-1 encode task. PRIVATE: the task's lifetime is owned by the engage predicate in
    // prepare(), and renderSplitActive_ is the single source of truth for "is the split on". An outside
    // caller could desync the two — stop the task while the flag stays true, and tick() would notify a
    // dead task while quiesceEncode() waited on an encodeDone_ nobody will ever set. Safe to call again
    // (guards on encodeTask_.impl). Degrades to inline if the task can't be created.
    void startEncodeTask() {
        if (!renderSplitActive_ || encodeTask_.impl) return;
        encodeStop_.store(false, std::memory_order_release);
        encodeDone_.store(true, std::memory_order_release);
        // Core 1, priority 5, 8 KB — matches the OTA/improv worker precedent; drivers own their DMA
        // buffers so the task stack is light.
        if (!platform::spawnPinnedTask(encodeTask_, "mmEncode", &encodeTrampoline, this, 8192, 5, 1))
            renderSplitActive_ = false;   // couldn't create the task → inline path
    }
    // Stop + join the core-1 task, draining its in-flight encode before any buffer it reads is freed.
    // Reached from release(), the destructor, disengage, and a timed-out quiesce. Safe when idle.
    void stopEncodeTask() {
        if (!encodeTask_.impl) return;
        encodeStop_.store(true, std::memory_order_release);
        platform::stopPinnedTask(encodeTask_);   // signals + wakes + joins (worker exits its loop)
    }

    void passBufferToDrivers() {
        // No active Layer (e.g. the last Layer was just deleted): clear every
        // driver's Layer + source-buffer pointers rather than leaving them at
        // their previous values. An early return here left drivers holding a
        // dangling layer_ pointing at the freed Layer — PreviewDriver then read
        // layer_->layouts() on freed memory and crashed (LoadProhibited). A
        // driver with a null layer/buffer is a well-defined idle state.
        // Drivers read outputBuffer_ whenever prepare() allocated one — it did so because we
        // composite (≥2 enabled layers), must LUT-map a single layer, OR the multicore split needs a
        // stable frame for core 1. Otherwise (no buffer: the lone no-LUT layer with the split off, or
        // an allocation that degraded) the layer's own buffer is handed directly — the zero-copy fast
        // path. Keying off `outputBuffer_.data()` rather than re-deriving the reason keeps ONE
        // decision (prepare's) instead of two that can disagree — a driver pointed at the layer buffer
        // while the split encodes from outputBuffer_ would output a stale frame.
        // The source is the first *enabled* layer, never the disabled fallback activeLayer() returns
        // when all layers are off — with no enabled layer buf stays null and every driver idles (its
        // last frame is not re-sent). A pinned setLayer() (effects_ null) is always the live source.
        Layer* const out = effects_ ? effects_->firstEnabledLayer() : layer_;
        Buffer* buf = out ? (outputBuffer_.data() ? &outputBuffer_ : &out->buffer())
                          : nullptr;
        for (uint8_t i = 0; i < childCount(); i++) {
            // Skip the non-driver child (the LightPresetsModule, role Generic): it has no source
            // buffer / correction to wire — only output drivers do.
            if (child(i)->role() != ModuleRole::Driver) continue;
            auto* drv = static_cast<DriverBase*>(child(i));
            drv->setSourceBuffer(buf);
            // Geometry uses layer_ (activeLayer()'s fallback — valid even when every
            // layer is disabled) so a PreviewDriver keeps its width/height/depth and
            // coordinate table; buf above uses the enabled source only, so output
            // still idles (no stale frame) when nothing is enabled. layer_ is null
            // only when no Layer is registered at all (the documented idle state).
            drv->setLayer(layer_);
            // Each driver owns its correction; push the current global brightness so it can
            // bake global × its local brightness into its own LUT (physical drivers apply it,
            // Preview ignores). A driver's own correction-control edits rebuild it themselves.
            drv->rebuildCorrection(effectiveBrightness());
        }
    }
};

} // namespace mm
