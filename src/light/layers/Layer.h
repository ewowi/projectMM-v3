#pragma once

#include "light/FixtureChannels.h"   // motion-channel offsets an effect writes through
#include "light/layers/Buffer.h"
#include "light/layouts/Layouts.h"
#include "light/effects/EffectBase.h"
#include "light/layers/MappingLUT.h"
#include "light/layers/BlendMap.h"   // BlendOp, for blendOp()
#include "light/modifiers/ModifierBase.h"
#include "light/draw.h"              // draw::fade — the once-per-frame collected fade (fadeToBlackBy)
#include "light/particles.h"       // particles::FrameTime, the shared elapsed-to-scale conversion
#include "platform/platform.h"

#include <cstdio>
#include <cstring>  // std::memcpy in extrude()

namespace mm {

/// A `Layer` MoonModule (role `ModuleRole::Layer`, child of the `Effects` container) owns a buffer, a mapping LUT, an ordered effect list, and an ordered modifier list, and references the shared `Layouts` that describes the physical topology.
///
/// **Ownership:** a `Buffer` (logical light data, sized to the logical box); a `MappingLUT` (logical lights to physical positions); effects (write lights into the buffer, dynamic heap-grown list, no fixed max); modifiers (transform the LUT or light values, same dynamic list).
///
/// **Composition:** two controls, `blendMode` and `opacity`, govern how this Layer composites onto the layers below it. They are inert on the Layer — it never reads them; a Layer can't know its position in the stack or what's beneath it. The `Drivers` container reads each enabled Layer's two values plus the container child order and does the compositing (bottom layer overwrites, each layer above blends per its mode and opacity). The value lives here so it travels with the Layer through add / delete / reorder — no separate sync-prone blend list on Drivers. The blend math itself lives in `BlendMap`.
///
/// **Buffer persistence:** the buffer persists frame-to-frame — the Layer does NOT clear it. This is the FastLED / WLED / MoonLight convention: the buffer holds the previous frame so an effect can fade it for trails (`fadeToBlackBy`) or read prior pixels (a scroll, Game-of-Life). Each effect owns its background. `rebuildLUT` clears once on the cold path so a freshly added effect starts black.
///
/// **rebuildLUT (cold path):** called when a layout or modifier control changes. Reads physical dimensions from `Layouts`, folds the box through each enabled static modifier to compute the logical box, allocates the buffer and LUT, and for the common case (no modifier, dense grid in natural order) skips the table entirely with an identity mapping. The general path folds each physical light through the static chain to its logical cell via a textbook counting-sort CSR build.
///
/// **render (hot path):** runs each enabled effect in order (all write the same buffer), calls `extrude` after each effect to duplicate its written slice across the axes it doesn't iterate, then ticks each enabled modifier. A live (animated) modifier triggers the per-frame `applyLivePass` backward gather; a beat-driven one asks for a single coalesced rebuild.
///
/// **extrude:** lets a low-dimensional effect work on a higher-dimensional layer without per-effect changes — a D2 effect on a 3D layer has its z=0 slice copied across z, a D1 effect its x=0 column copied across x, then that row across the rest. Cost is zero for D3 effects (the default, an early return) and zero when the layer's unused axes are size 1 (a D2 effect on a 2D layer). Real `memcpy` work only happens when the layer has more dimensions than the effect writes.
///
/// **Status:** the status slot shows the LOGICAL box the effects render into (`` `<w>×<h>×<d>` ``), which can differ from the physical box shown on `Layouts` (a Mirror-XY modifier folds a 128×128 physical layout into a 64×64 logical box). The same slot carries memory-degradation warnings when a build can't fit (`modifier mapping skipped`, `buffer reduced`, `buffer allocation failed`, all `— not enough memory`), and a warning wins over the neutral box line.
///
/// **Prior art:** MoonLight's `VirtualLayer` — `oneToOneMapping` fast-path flag, `virtualChannels` per-layer buffer, `effectDimension`, a `nodes` vector for effects/modifiers, and `forEachLight` per-logical-light iteration that asks the modifier for physical destinations (https://github.com/ewowi/MoonLight/blob/main/src/MoonLight/Layers/VirtualLayer.h).
/// @card Layer.png
class Layer : public MoonModule {
public:
    ModuleRole role() const MM_NONBLOCKING override { return ModuleRole::Layer; }
    const char* acceptsChildRoles() const override { return "effect,modifier"; }

    ~Layer() override { if (liveScratch_) platform::free(liveScratch_); }


    // Composition parameters — INERT on the Layer (it never reads them; a Layer
    // can't know its position in the stack or what's beneath it). The Drivers
    // container reads each enabled Layer's blendMode + opacity and composites the
    // layers in container order into the physical buffer (see Drivers::tick). The
    // value lives here so it travels with the Layer through add/delete/reorder —
    // no separate, sync-prone blend list on Drivers. The bottom (first-composited)
    // layer's blendMode is moot: it fills the cleared buffer regardless.
    // Default additive (index 1): a newly-added layer ADDS light onto the layers below and never
    // blacks them out, which matches the common case (a sparse effect — sparks, a comet, text —
    // stacked over a background). Alpha (over, index 0) is opt-in for full-frame layers that MEAN to
    // cover what's below, where its black pixels are intended, not a surprise. Index order is fixed by
    // kBlendModeOptions (alpha=0, additive=1) so a persisted preset's stored index keeps its meaning.
    uint8_t blendMode = 1;     // index into kBlendModeOptions; 1 = additive
    uint8_t opacity = 255;     // 0 = invisible, 255 = full

    void defineControls() override {
        static constexpr const char* kBlendModeOptions[] = {"alpha", "additive"};
        controls_.addSelect("blendMode", blendMode, kBlendModeOptions, 2);
        controls_.addControl("opacity", opacity, 0, 255);
        // Cascade to children (effects and modifiers) — preserves the default
        // base behaviour we just overrode.
        MoonModule::defineControls();
    }

    /// How this Layer composites when stacked above another (read by Drivers).
    /// Maps the blendMode select index to the BlendMap op. Index order must match
    /// kBlendModeOptions above.
    BlendOp blendOp() const {
        return blendMode == 1 ? BlendOp::Additive : BlendOp::Alpha;
    }

    void setLayouts(Layouts* lg) { layouts_ = lg; }
    // The active Layouts, for consumers that need per-light coordinates (e.g.
    // PreviewDriver builds its coordinate table from layouts()->placeLights).
    Layouts* layouts() const { return layouts_; }
    /// Channels per light (3 = RGB, 4 = RGBW, more for fixture profiles). Zero is not a valid
    /// light: it would allocate a zero-byte buffer and make every effect's per-light stride 0, so
    /// it is rejected here rather than defended against downstream. Enforcing the invariant at the
    /// one entry point is what lets effects and draw primitives assume `cpl >= 1`.
    void setChannelsPerLight(uint8_t cpl) { if (cpl > 0) channelsPerLight_ = cpl; }

    /// Where this layer's fixtures keep their motion channels (pan/tilt/zoom/...). Set by whoever
    /// knows the fixture profile; every offset is absent by default, so an effect's setPan() is a
    /// harmless no-op on a plain LED strip.
    void setFixtureChannels(const FixtureChannels& fc) { fixture_ = fc; }
    const FixtureChannels& fixtureChannels() const { return fixture_; }

    void prepare() override {
        // Restart discards the elapsed gap. Without this the first tick after a re-prepare sees the
        // whole idle interval as one step and jumps the trail forward: the guarantee
        // LissajousEffect::prepare used to give for its own trail, now given once for every effect.
        fadeTime_.reset();
        fadeCarry_ = 0;
        // Treat "no layouts wired" the same as "every layout child disabled" —
        // either way the Layer should be empty (no LUT, no buffer, zero dims).
        // Returning early here used to leave stale state from a previous build,
        // which Drivers then read as a sized LUT pointing at a null buffer.
        const nrOfLightsType physicalCount = layouts_ ? layouts_->totalLightCount() : 0;

        // Empty layout (every layout child disabled, or no layouts wired): tear
        // down the LUT and buffer and report zero dims. Bailing out without
        // dropping the old state left the LUT sized for the previous layout
        // while Drivers reallocated its output buffer to 0 bytes (a stale LUT
        // + null output buffer = blendMap dereferences null on the next tick).
        // After this branch hasLUT() is false and physicalLightCount() is 0,
        // so Drivers::prepare takes the "no LUT" path and Drivers::tick
        // skips blendMap entirely.
        if (physicalCount == 0) {
            physicalWidth_ = physicalHeight_ = physicalDepth_ = 0;
            width_ = height_ = depth_ = 0;
            lut_.free();
            buffer_.free();
            setDynamicBytes(0);
            // Clear stale degrade state from a previous build — both the status
            // string AND lutSkipped_. Without resetting the flag, lutSkipped()
            // keeps reporting true even though we just freed the LUT.
            lutSkipped_ = false;
            clearStatus();
            return;   // applyState() recurses to the effects next
        }

        // Compute physical dimensions from layout. Gaps count toward the box (a black pixel occupies a
        // real position), so one callback handles both kinds — blackCb null → blackPixel falls back.
        struct DimCtx { lengthType maxX, maxY, maxZ; };
        DimCtx dctx{0, 0, 0};
        layouts_->placeLights(CoordSink{[](void* ctx, nrOfLightsType, lengthType x, lengthType y, lengthType z) {
            auto* d = static_cast<DimCtx*>(ctx);
            if (x > d->maxX) d->maxX = x;
            if (y > d->maxY) d->maxY = y;
            if (z > d->maxZ) d->maxZ = z;
        }, nullptr, &dctx});
        physicalWidth_ = dctx.maxX + 1;
        physicalHeight_ = dctx.maxY + 1;
        physicalDepth_ = dctx.maxZ + 1;

        rebuildLUT();
        // Start from a clean frame on every (re)build: adding, replacing, or reconfiguring an effect
        // rebuilds the Layer, and the buffer no longer clears per frame (it persists for trails/scroll)
        // — so without this a freshly added effect would inherit the previous effect's last frame. One
        // clear here (cold path, not per frame) means a new effect starts black; then persistence takes
        // over frame to frame.
        buffer_.clear();
        ensureLiveScratch();   // size the live-pass snapshot here, on the cold path

        // Neutral status: the LOGICAL box the effects render into (width_×height_×
        // depth_) — this is what start/end region carving and modifiers reshape,
        // so it can differ from the physical box (shown on Layouts). Only set it
        // when rebuildLUT left the status clear; a degrade path (LUT skipped /
        // buffer reduced) sets its own Warning, which must win over this line.
        if (status() == nullptr) {
            std::snprintf(statusBuf_, sizeof(statusBuf_), "%u×%u×%u",
                          static_cast<unsigned>(width_),
                          static_cast<unsigned>(height_),
                          static_cast<unsigned>(depth_));
            setStatus(statusBuf_);
        }

        // applyState() recurses to the effects next — they allocate against the LUT/buffer just built.
    }

    void tick() MM_NONBLOCKING override {
        // Scheduler already gates the Layer itself by enabled() via respectsEnabled().
        // We still gate per-effect-child explicitly because Layer iterates its own
        // children rather than going through the Scheduler.
        //
        elapsed_ = platform::millis();
        // The buffer PERSISTS frame-to-frame — the Layer does NOT clear it. This is the FastLED /
        // WLED / MoonLight convention: the buffer holds the previous frame so an effect can fade it
        // for trails (fadeToBlackBy, a "tail" control) or read prior pixels (draw::get, Game-of-Life,
        // a scroll). Each effect owns its background: a full-grid effect overwrites every pixel, a
        // trail effect fades then paints, a sparse effect that wants a clean frame calls draw::fill
        // itself. An auto-clear here would make trails and read-prior effects impossible.
        //
        // Consume the collected fade ONCE per frame, before the effects run — the MoonLight model
        // (VirtualLayer): effects call layer()->fadeToBlackBy(amt) which MINs into fadeBy_, so N
        // fading effects on one layer cost ONE buffer pass (the gentlest amount wins, preserving the
        // most light / longest trail) instead of each effect fading the whole shared buffer itself.
        // Scale the requested RATE by the fraction of a reference frame this frame covered, and
        // CARRY the remainder rather than flooring it to 1: at high frame rates the per-frame
        // amount is legitimately below one unit, and a floor of 1 would apply many times the decay
        // the effect asked for, which is the bug that made trails visibly shorter on a fast device.
        // ALWAYS advance the clock, even on a frame nobody asked to fade. Only a quarter of the
        // effects fade at all, so leaving it frozen means the next request sees the whole idle gap
        // as one step: five seconds away and a gentle trail is wiped black in a single frame. That
        // is reachable by switching to a fading effect, re-enabling one, or resuming StarField,
        // whose paused path returns before it asks.
        const uint32_t frameScale = fadeTime_.advance(elapsed_);
        if (fadeBy_ > 0) {
            fadeCarry_ += static_cast<uint32_t>(fadeBy_) * frameScale;
            uint32_t amt = fadeCarry_ / particles::FrameTime::kOne;
            fadeBy_ = 0;
            // A stall TOPS UP, it never bursts: spending a whole gap at once is the wipe described
            // above. Dropping the remainder with it keeps the next frame from repeating the burst.
            if (amt > 255) {
                amt = 255;
                fadeCarry_ = 0;              // the gap is spent, not banked for the next frame
            } else {
                fadeCarry_ -= amt * particles::FrameTime::kOne;
            }
            if (amt > 0) {
                draw::fade(buffer_, static_cast<uint8_t>(amt));
                bufferGen_++;
            }
        }
        // A degenerate grid has nothing to draw. This is orchestration — the Layer owns the
        // decision to run the effect pass at all, the same way it owns the enabled/role gates
        // below — so it is checked ONCE here rather than repeated as a guard clause in every
        // effect's tick(). Effects may assume width/height/depth are all >= 1.
        //
        // It gates only the EFFECT pass, not the whole tick: the modifier pass below advances
        // per-frame state (a beat-driven RandomMap) that must keep running so the chain is in
        // the right phase when the grid comes back.
        const bool hasGrid = width_ > 0 && height_ > 0 && depth_ > 0 && buffer_.count() > 0;
        for (uint8_t i = 0; hasGrid && i < childCount(); i++) {
            if (child(i)->role() != ModuleRole::Effect) continue;
            if (!child(i)->enabled()) continue;
            auto* eff = static_cast<EffectBase*>(child(i));
            uint32_t start = platform::micros();
            eff->tick();
            // Extrude a lower-dimensional effect across the unused axes so a D1
            // or D2 effect "just works" on a higher-dimensional grid. The effect
            // only writes its own slice (D1 → column x=0,z=0; D2 → slice z=0); the
            // framework duplicates that across the rest of the buffer.
            extrude(eff->dimensions());
            bufferGen_++;   // this effect wrote the shared buffer; see bufferGen()
            eff->addAccumUs(platform::micros() - start);
        }
        // Tick EVERY enabled modifier AFTER the effect pass (the frame's buffer is
        // fully written before any modifier acts). A static modifier's tick() is empty;
        // a beat-driven one (RandomMap) sets a rebuild flag we coalesce below; a live
        // one (Rotate) advances its angle here and remaps in the live pass that follows.
        bool rebuild = false;
        for (uint8_t i = 0; i < childCount(); i++) {
            if (child(i)->role() != ModuleRole::Modifier || !child(i)->enabled()) continue;
            auto* m = static_cast<ModifierBase*>(child(i));
            m->tick();
            rebuild |= m->consumeNeedsRebuild();
        }
        // One rebuild per frame even if several modifiers asked (no re-entrant rebuild
        // from inside a modifier's tick()). applyState() rebuilds the whole pipeline —
        // re-runs rebuildLUT() with the modifiers' fresh state, then recurses to the effects.
        if (rebuild) { applyState(); return; }

        // Live pass: remap the logical buffer per frame for dynamic modifiers (Rotate).
        // Skipped entirely when no modifier is live — a static-only chain pays nothing,
        // the buffer goes straight to the driver scatter (the pay-for-what-you-use rule).
        // hasGrid too: applyLivePass walks the mapping into the buffer, and an empty layout has
        // neither. The effect pass above is already gated the same way.
        if (hasGrid && hasLive_) { applyLivePass(); bufferGen_++; }
    }

    // COLD path (called from prepare after rebuildLUT): (re)size the live-pass
    // snapshot buffer to the current logical buffer, or free it when no modifier is live.
    // Keeping the alloc here means applyLivePass() on the render path only memcpys —
    // never allocates — and the scratch isn't held pinned once live modifiers are removed.
    void ensureLiveScratch() {
        const size_t bytes = hasLive_ ? buffer_.bytes() : 0;
        if (bytes == liveScratchBytes_ && (bytes != 0) == (liveScratch_ != nullptr)) return;
        if (liveScratch_) { platform::free(liveScratch_); liveScratch_ = nullptr; }
        liveScratchBytes_ = 0;
        if (bytes == 0) return;                       // no live modifier → no scratch held
        liveScratch_ = static_cast<uint8_t*>(platform::alloc(bytes));
        if (liveScratch_) liveScratchBytes_ = bytes;  // alloc-fail → applyLivePass no-ops, static frame shows
    }

    // Per-frame backward gather for live (animated) modifiers. For each DESTINATION
    // logical cell, fold its coordinate through the enabled live modifiers to the SOURCE
    // cell it samples, and copy that source pixel in — so no destination is left torn
    // (backward mapping, the textbook reason image warping samples backward). Reads from
    // a snapshot (liveScratch_) so a source already overwritten this pass isn't re-read.
    // Out-of-box sources leave the destination dark (cleared). Cold relative to the build
    // but on the hot path — runs only because hasLive_ gated it, and only the live
    // modifiers participate (static ones are already baked into lut_).
    void applyLivePass() {
        uint8_t* buf = buffer_.data();
        if (!buf || !liveScratch_) return;   // scratch is sized on the cold path (ensureLiveScratch)
        const size_t cpl = channelsPerLight_;
        const size_t bytes = static_cast<size_t>(width_) * height_ * depth_ * cpl;
        if (bytes == 0 || bytes > liveScratchBytes_) return;   // hot path NEVER allocates
        std::memcpy(liveScratch_, buf, bytes);   // snapshot the source frame

        const Coord3D logical{width_, height_, depth_};
        for (lengthType z = 0; z < depth_; z++) {
            for (lengthType y = 0; y < height_; y++) {
                for (lengthType x = 0; x < width_; x++) {
                    Coord3D src{x, y, z};
                    for (uint8_t i = 0; i < childCount(); i++) {
                        if (child(i)->role() != ModuleRole::Modifier || !child(i)->enabled()) continue;
                        auto* m = static_cast<ModifierBase*>(child(i));
                        if (m->hasModifyLive()) m->modifyLive(src, logical);
                    }
                    const size_t dstIdx = (static_cast<size_t>(z) * height_ * width_ +
                                           static_cast<size_t>(y) * width_ + x) * cpl;
                    if (src.x >= 0 && src.x < width_ && src.y >= 0 && src.y < height_ &&
                        src.z >= 0 && src.z < depth_) {
                        const size_t srcIdx = (static_cast<size_t>(src.z) * height_ * width_ +
                                               static_cast<size_t>(src.y) * width_ + src.x) * cpl;
                        std::memcpy(buf + dstIdx, liveScratch_ + srcIdx, cpl);
                    } else {
                        std::memset(buf + dstIdx, 0, cpl);   // source off-box → dark
                    }
                }
            }
        }
    }

    /// Copy the effect's written slice to fill the unused axes. Called after each
    /// effect's tick(). Buffer layout is (z * h + y) * w + x channels per light.
    ///
    /// Hot-path shape: D3 effects (the default) take the early return and pay
    /// nothing beyond one comparison and a branch. On a 2D layout (depth=1) the
    /// z-fill is naturally a no-op regardless of effectDim — the `` `depth_ > 1` ``
    /// guard short-circuits. Same for D1 on a 1D layout. Real work only happens
    /// when the effect declared fewer axes than the layout has. See
    /// EffectBase § Dimensions and auto-extrusion for the effect-side contract.
    void extrude(Dim effectDim) {
        if (effectDim == Dim::D3) return;
        uint8_t* buf = buffer_.data();
        if (!buf) return;
        const size_t cpl = channelsPerLight_;
        const size_t rowBytes = static_cast<size_t>(width_) * cpl;
        const size_t sliceBytes = rowBytes * height_;

        // 1D runs along Y: a D1 effect wrote the (x=0) column down y in z=0. Duplicate that column
        // across all x > 0, so a 1D effect expands into 2D by *adding columns to the right* — the
        // 1D output is literally the first column of its 2D form (see architecture.md §
        // Dimensionality). cpl bytes per pixel copied from the x=0 pixel of each row.
        if (effectDim == Dim::D1 && width_ > 1) {
            for (lengthType y = 0; y < height_; y++) {
                const uint8_t* src = buf + static_cast<size_t>(y) * rowBytes;   // the x=0 pixel
                for (lengthType x = 1; x < width_; x++) {
                    std::memcpy(buf + static_cast<size_t>(y) * rowBytes + static_cast<size_t>(x) * cpl,
                                src, cpl);
                }
            }
        }
        // D1 and D2: z=0 now holds a complete (possibly extruded) slice — the (x,y) front face.
        // Duplicate it across all z > 0, so a 2D effect expands into 3D by adding depth slices.
        if (depth_ > 1) {
            for (lengthType z = 1; z < depth_; z++) {
                std::memcpy(buf + z * sliceBytes, buf, sliceBytes);
            }
        }
    }

    Buffer& buffer() { return buffer_; }
    const Buffer& buffer() const { return buffer_; }
    const MappingLUT& lut() const { return lut_; }

    // Effects see logical dimensions
    lengthType width() const { return width_; }
    lengthType height() const { return height_; }
    lengthType depth() const { return depth_; }
    uint8_t channelsPerLight() const { return channelsPerLight_; }
    uint32_t elapsed() const { return elapsed_; }

    // Request a fade-to-black of amt/255 PER REFERENCE FRAME (1/60 s): a trail or tail. Effects call
    // this instead of fading the buffer themselves: the Layer collects the amount (MIN across all
    // fading effects, the gentlest fade wins so the longest requested trail is honoured) and applies
    // ONE buffer pass at the start of the next frame, then resets. MoonLight's
    // VirtualLayer::fadeToBlackBy model: N fading effects on one layer cost one pass, not N, and
    // never fade each other's fresh pixels.
    //
    // The amount is a RATE, not a per-frame constant. The Layer scales it by the time this frame
    // actually covered, so a trail is the same length on a 470 fps ESP32 and a 140,000 fps desktop.
    // Three effects used to carry that conversion themselves and had already drifted into two
    // different versions of it (one carried the fraction, two floored to 1 and so applied many
    // times the intended decay at high fps). Owning it here is core enforcing the rule on the path
    // it already owns rather than every effect re-deriving it. See architecture.md, the tick-rate
    // rule, and particles::FrameTime for the shared conversion.
    //
    // Every amount is a rate, with no exception. An effect that wants the buffer blank NOW calls
    // draw::fill instead: a clear is not a fast fade, and giving 255 a second meaning put a
    // discontinuity in kind at the top of six user-facing fade sliders.
    void fadeToBlackBy(uint8_t amt) { fadeBy_ = fadeBy_ ? (amt < fadeBy_ ? amt : fadeBy_) : amt; }

    /// How many times anything has written this layer's shared buffer. The buffer PERSISTS between
    /// frames (see tick), so an effect that holds a previous frame — a network receiver, a still —
    /// can re-lay it only when something actually disturbed it, instead of re-copying every tick.
    /// Bumped by the collected fade, by every effect's tick, and by the live-modifier pass, so
    /// "unchanged" means the bytes are exactly as that effect left them. A new writer of `buffer_`
    /// must bump it too, the same discipline the fade follows.
    uint32_t bufferGen() const { return bufferGen_; }

    nrOfLightsType physicalLightCount() const {
        return layouts_ ? layouts_->totalLightCount() : 0;
    }

    // Physical dimensions match the actual LED arrangement (computed in prepare from
    // the Layouts). PreviewDriver and any future driver that needs to describe the LED
    // shape should read these rather than caching values from main.cpp startup.
    lengthType physicalWidth() const { return physicalWidth_; }
    lengthType physicalHeight() const { return physicalHeight_; }
    lengthType physicalDepth() const { return physicalDepth_; }

    bool lutSkipped() const { return lutSkipped_; }

    /// Cold path, called from prepare after physical dimensions are known.
    /// Applies each enabled static modifier to compute the logical box, allocates
    /// the buffer and LUT, and for each logical light asks the modifier chain for
    /// physical destinations. Without a modifier AND with a dense grid in natural
    /// order (no sparse, no serpentine, x-then-y-then-z) it sets an identity mapping
    /// and skips the table entirely (the FPS floor for the common case).
    /// Precondition: physicalWidth_/Height_/Depth_ must be set (call from prepare).
    void rebuildLUT() {
        lutSkipped_ = false;
        clearStatus();  // re-evaluated below if a degrade path is taken

        // Fold the box through each enabled STATIC modifier in child order — no fixed
        // chain array (Dynamic over fixed-size, architecture.md): the size pass here and
        // the per-light fold below both iterate the Layer's own (dynamic, heap-grown)
        // child list, filtering for enabled static modifiers inline, the way MoonLight's
        // `for node : nodes` does. modifyLogicalSize mutates the running box AND lets the
        // modifier stash its own output size (MoonLight's modifierSize cache), so in the
        // per-light fold each modifier reads the box at ITS OWN stage from itself.
        // A dynamic modifier (Rotate, hasModifyLive) is excluded — it remaps per frame in
        // Layer::tick's live pass, not baked into the mapping.
        uint8_t staticCount = 0;
        hasLive_ = false;
        Coord3D box{physicalWidth_, physicalHeight_, physicalDepth_};
        for (uint8_t i = 0; i < childCount(); i++) {
            if (child(i)->role() != ModuleRole::Modifier || !child(i)->enabled()) continue;
            auto* m = static_cast<ModifierBase*>(child(i));
            if (m->hasModifyLive()) { hasLive_ = true; continue; }   // dynamic: per-frame, not baked
            m->modifyLogicalSize(box);
            clampLogical(box);
            staticCount++;
        }

        // Final logical box = the running box after the last static modifier.
        Coord3D logical = box;
        width_ = logical.x; height_ = logical.y; depth_ = logical.z;

        const Coord3D phys{physicalWidth_, physicalHeight_, physicalDepth_};
        const nrOfLightsType boxCount    = cellCount(phys);
        const nrOfLightsType logicalCount = cellCount(logical);
        const nrOfLightsType driverCount = physicalLightCount();   // == Layouts::totalLightCount()
        const bool dense = (driverCount == boxCount);
        // A gap fills a box cell (dense stays true) but must NOT receive that cell's color, so the
        // identity map — which lights every cell — is wrong when gaps exist. Route to the folded
        // build, which drops the gap slots from the LUT (they stay black). No gaps → unchanged.
        const bool anyGap = layouts_ && layouts_->hasBlackPixels();

        // Fast path — no static modifiers, dense grid in natural order, no gaps: box cell i
        // IS driver light i, so the mapping is the identity memcpy. This is the FPS
        // floor for the common case; keep it before any allocation.
        if (staticCount == 0 && dense && !anyGap && isNaturalOrder()) {
            lut_.setIdentity(boxCount);
            allocateBuffer(boxCount);
            return;
        }

        // General build — fold each PHYSICAL light through the static chain to its
        // logical cell, accumulating the physical (driver) index onto that cell.
        // N physical lights folding onto one logical cell IS the fan-out (Multiply),
        // so each physical light contributes at most ONE destination — maxDest is
        // exactly driverCount, no product, no overflow ceiling.
        if (!buildFoldedLUT(logical, logicalCount, driverCount)) {
            // OOM in the fold build — degrade to identity (safe, not crash). One visual caveat: a
            // gapped layout's dark columns light up in this degraded state (the identity map has no
            // way to drop them), but a Warning is surfaced and the device keeps running — the
            // robustness principle's "degraded, not crashed" applied to an out-of-memory build.
            lutSkipped_ = true;
            setStatus("modifier mapping skipped — not enough memory", Severity::Warning);
            width_ = physicalWidth_; height_ = physicalHeight_; depth_ = physicalDepth_;
            lut_.setIdentity(boxCount);
            allocateBuffer(boxCount);
            return;
        }
        allocateBuffer(logicalCount);
    }

    // Sentinel: a box cell that is not a real light (no driver index).
    static constexpr nrOfLightsType kNoDriver = static_cast<nrOfLightsType>(-1);

    // Does the layout emit lights in natural box order — driver index i == box cell i (x fastest,
    // then y, then z)? Measured, not declared: one allocation-free placeLights pass over the same
    // coords the build would walk, so there's a single source of truth (the coords) and no
    // per-layout hint to keep in sync. True → the dense memcpy fast path is valid; false → a
    // reordered grid (serpentine) needs the folded LUT. Only meaningful for a dense layout
    // (boxCount == driverCount); a sparse layout always routes to the folded build.
    bool isNaturalOrder() const {
        struct Ctx { lengthType w, h; bool ok; };
        Ctx ctx{physicalWidth_, physicalHeight_, true};
        // Only reached for a gap-free layout (a gap routes to the folded build before this is asked),
        // so blackCb is null and gaps, were there any, would fall back to the same order check.
        layouts_->placeLights(CoordSink{[](void* c, nrOfLightsType driverIdx, lengthType x, lengthType y, lengthType z) {
            auto* k = static_cast<Ctx*>(c);
            if (!k->ok) return;   // once a mismatch is found the answer is settled; skip the rest
            nrOfLightsType box = static_cast<nrOfLightsType>(z) * k->w * k->h
                               + static_cast<nrOfLightsType>(y) * k->w + x;
            if (driverIdx != box) k->ok = false;
        }, nullptr, &ctx});
        return ctx.ok;
    }

    // Build the mapping by folding PHYSICAL lights to LOGICAL cells (physical→logical).
    // Our MappingLUT is a CSR keyed by logical index, and setMapping demands sequential
    // in-order writes — but folding scatters onto arbitrary, repeated logical indices.
    // So this is the textbook counting-sort CSR build: pass A counts destinations per
    // logical cell, prefix-sum to offsets, pass B scatters, then replay through
    // setMapping in logical order. Two placeLights passes + a counts/dests scratch,
    // all on the cold rebuild path; the hot-path read (forEachDestination) is unchanged.
    // Returns false on OOM (caller degrades to identity).
    bool buildFoldedLUT(const Coord3D& logical,
                        nrOfLightsType logicalCount, nrOfLightsType driverCount) {
        if (logicalCount == 0 || driverCount == 0) { lut_.setIdentity(0); return true; }

        // Scratch: per-logical-cell counts (then reused as the write cursor) and the
        // scattered driver indices. Each physical light yields ≤1 destination, so the
        // dests array is driverCount-sized — the tight, overflow-free ceiling.
        auto* counts = static_cast<nrOfLightsType*>(
            platform::alloc(static_cast<size_t>(logicalCount + 1) * sizeof(nrOfLightsType)));
        auto* dests = static_cast<nrOfLightsType*>(
            platform::alloc(static_cast<size_t>(driverCount) * sizeof(nrOfLightsType)));
        if (!counts || !dests) {
            if (counts) platform::free(counts);
            if (dests) platform::free(dests);
            return false;
        }
        for (nrOfLightsType i = 0; i <= logicalCount; i++) counts[i] = 0;

        // One callback does both passes. It folds the physical coord through the chain
        // (the Layer's own children — enabled static modifiers, in order, no array) to a
        // logical index (or skips it if a modifier rejects it or it lands out of box —
        // guarded, never trusted), then either counts it (pass A) or writes the driver
        // index at the cell's cursor (pass B). Everything travels through the placeLights
        // void* ctx, so the lambda captures nothing (it's a function ptr).
        struct FoldCtx {
            Layer* self;   // for the dynamic child list (the modifier chain)
            Coord3D logical; nrOfLightsType logicalCount;  // final box, for the flatten + guard
            nrOfLightsType* counts;   // pass A: per-cell count.  pass B: per-cell write cursor.
            nrOfLightsType* dests;    // pass B only.
            nrOfLightsType destCap;   // what dests actually holds — pass B must not exceed it.
            bool scatter;
        } fctx{this, logical, logicalCount, counts, dests, driverCount, /*scatter=*/false};

        auto onCoord = [](void* c, nrOfLightsType driverIdx, lengthType x, lengthType y, lengthType z) {
            auto* f = static_cast<FoldCtx*>(c);
            Coord3D pos{x, y, z};
            Layer* self = f->self;
            for (uint8_t i = 0; i < self->childCount(); i++) {
                if (self->child(i)->role() != ModuleRole::Modifier || !self->child(i)->enabled()) continue;
                auto* m = static_cast<ModifierBase*>(self->child(i));
                if (m->hasModifyLive()) continue;                 // dynamic: not in the static fold
                if (!m->modifyLogical(pos)) return;               // rejected — no logical source
            }
            if (pos.x < 0 || pos.x >= f->logical.x || pos.y < 0 || pos.y >= f->logical.y ||
                pos.z < 0 || pos.z >= f->logical.z) return;                          // defensive
            const nrOfLightsType li =
                static_cast<nrOfLightsType>(pos.z) * static_cast<nrOfLightsType>(f->logical.x) * static_cast<nrOfLightsType>(f->logical.y) +
                static_cast<nrOfLightsType>(pos.y) * static_cast<nrOfLightsType>(f->logical.x) +
                static_cast<nrOfLightsType>(pos.x);
            if (li >= f->logicalCount) return;                                       // defensive
            // Pass B writes where pass A counted — safe only while both passes see the SAME
            // coordinates. A scripted layout compiles lazily inside placeLights, so a control
            // edited between the two passes makes pass B emit more lights than pass A counted and
            // the scatter runs past dests. That corrupts the heap; the failure then surfaces in an
            // unrelated allocation, which is what made resizing a scripted layout crash at random.
            // The bound makes a disagreement cost a dropped destination, never memory.
            if (f->scatter) {
                const nrOfLightsType slot = f->counts[li];
                if (slot >= f->destCap) return;
                f->dests[slot] = driverIdx;
                f->counts[li]++;
            } else {
                f->counts[li]++;                                    // pass A: bump the count
            }
        };

        // A GAP (black pixel) is DROPPED from the LUT: its physical slot is already counted in
        // driverCount, but no logical cell maps to it, so the scatter never writes it and it stays
        // black (blendMap clears first). The black handler is therefore a no-op — this is exactly the
        // "physical pixel that stays black" the feature is: a wire slot present, but no source. So both
        // passes use one sink whose blackCb does nothing.
        static constexpr CoordCallback kDropGap =
            [](void*, nrOfLightsType, lengthType, lengthType, lengthType) {};

        // Pass A — count.
        layouts_->placeLights(CoordSink{onCoord, kDropGap, &fctx});

        // Prefix-sum counts → offsets (counts[li] becomes the start of cell li's run).
        nrOfLightsType running = 0;
        for (nrOfLightsType i = 0; i < logicalCount; i++) {
            nrOfLightsType c = counts[i];
            counts[i] = running;
            running += c;
        }
        counts[logicalCount] = running;   // total destinations

        // Pass B — scatter. counts[] is now the per-cell write cursor (offsets advance).
        fctx.scatter = true;
        layouts_->placeLights(CoordSink{onCoord, kDropGap, &fctx});

        // Pass B advanced each cell's cursor to the END of its run, so counts[i] now
        // holds the end offset of cell i — which equals the START offset of cell i+1.
        // The run for cell i is therefore [counts[i-1], counts[i]) with counts[-1]=0,
        // i.e. the `start` cursor below. dests[] is already laid out in this exact CSR
        // order, so replaying it through setMapping in logical order is a straight copy.
        if (!lut_.build(logicalCount, running)) {   // running == total destinations
            platform::free(counts);
            platform::free(dests);
            return false;
        }
        nrOfLightsType start = 0;
        for (nrOfLightsType i = 0; i < logicalCount; i++) {
            nrOfLightsType end = counts[i];          // end of cell i's run
            lut_.setMapping(i, &dests[start], static_cast<nrOfLightsType>(end - start));
            start = end;
        }
        lut_.finalize();
        platform::free(counts);
        platform::free(dests);
        return true;
    }

    // Cells in a box (the flat light count). 0 on any 0-extent axis.
    static nrOfLightsType cellCount(const Coord3D& box) {
        return static_cast<nrOfLightsType>(box.x) * static_cast<nrOfLightsType>(box.y) *
               static_cast<nrOfLightsType>(box.z);
    }

    // A modifier's modifyLogicalSize must not collapse an axis the physical box has:
    // a 0-width logical box would blank the layer with no source for any effect. Clamp
    // each axis to ≥1 where the physical box is non-empty (keep a genuinely 0 axis 0).
    void clampLogical(Coord3D& logical) const {
        if (physicalWidth_  > 0 && logical.x < 1) logical.x = 1;
        if (physicalHeight_ > 0 && logical.y < 1) logical.y = 1;
        if (physicalDepth_  > 0 && logical.z < 1) logical.z = 1;
        if (logical.x < 0) logical.x = 0;
        if (logical.y < 0) logical.y = 0;
        if (logical.z < 0) logical.z = 0;
    }

private:
    Layouts* layouts_ = nullptr;
    Buffer buffer_;
    MappingLUT lut_;
    uint8_t channelsPerLight_ = 3;
    FixtureChannels fixture_;
    bool lutSkipped_ = false;
    lengthType physicalWidth_ = 0;
    lengthType physicalHeight_ = 0;
    lengthType physicalDepth_ = 0;
    lengthType width_ = 0;  // logical (what effects see)
    lengthType height_ = 0;
    lengthType depth_ = 0;
    uint32_t elapsed_ = 0;
    uint8_t  fadeBy_ = 0;   // fade RATE collected from effects (MIN), consumed once at frame start
    uint32_t fadeCarry_ = 0;               // sub-unit fade remainder, so a high frame rate does not over-fade
    particles::FrameTime fadeTime_{60};    // elapsed-to-scale, the shared conversion
    uint32_t bufferGen_ = 0;   // bumped by every write to buffer_; see bufferGen()
    char statusBuf_[20] = {};  // "999×999×999" fits; owned (setStatus borrows the pointer)
    bool     hasLive_ = false;          // any enabled modifier animates per frame (gates the live pass)
    uint8_t* liveScratch_ = nullptr;    // snapshot for the live pass; allocated only when hasLive_
    size_t   liveScratchBytes_ = 0;

    // Check if heap can afford an allocation (returns true if unlimited or enough budget)
    static bool canAllocate(size_t bytesNeeded) {
        size_t availableHeap = platform::freeHeap();
        if (availableHeap == 0) return true; // desktop: unlimited
        size_t internalHeap = platform::freeInternalHeap();
        if (internalHeap > 0 && internalHeap <= platform::HEAP_RESERVE) return false;
        size_t budget = availableHeap > platform::HEAP_RESERVE ? availableHeap - platform::HEAP_RESERVE : 0;
        return budget >= bytesNeeded && platform::maxAllocBlock() >= bytesNeeded;
    }

    /// The channel count this layer's fixtures need: RGBW plus one byte per motion role, or 0 when
    /// nothing in the rig moves. Read from the offsets Drivers derived from the light preset.
    uint8_t requiredChannels() const {
        const FixtureChannels& f = fixture_;
        uint8_t top = 0;
        for (uint8_t o : {f.pan, f.tilt, f.zoom, f.rotate, f.gobo})
            if (o != FixtureChannels::kAbsent && o + 1 > top) top = static_cast<uint8_t>(o + 1);
        return top;
    }

    void allocateBuffer(nrOfLightsType count) {
        // A light must be wide enough to hold the motion channels the rig's fixtures carry, or an
        // effect's setPan() writes past the end of the light and is silently dropped. Widening
        // HERE (cold path, before the allocation) rather than from Drivers is deliberate: changing
        // the width after the buffer exists resizes it under whoever is holding it, which segfaults.
        // A rig with no motion is untouched, so a plain LED strip keeps its 3 or 4 bytes per light.
        if (const uint8_t need = requiredChannels(); need > channelsPerLight_) channelsPerLight_ = need;

        // Try to allocate buffer, halve dimensions if needed
        bool reduced = false;
        while (count > 0) {
            size_t needed = static_cast<size_t>(count) * channelsPerLight_;
            if (canAllocate(needed)) {
                if (buffer_.allocate(count, channelsPerLight_)) {
                    setDynamicBytes(buffer_.bytes() + lut_.memoryUsed());
                    if (reduced) setStatus("buffer reduced — not enough memory", Severity::Warning);
                    return;
                }
                // allocate returned false despite canAllocate check — degrade
                std::printf("  DEGRADE  buffer_.allocate failed for %u lights\n",
                            static_cast<unsigned>(count));
            }
            // Halve: reduce to sqrt of count (halve each dimension)
            width_ = width_ > 1 ? width_ / 2 : 1;
            height_ = height_ > 1 ? height_ / 2 : 1;
            depth_ = depth_ > 1 ? depth_ / 2 : 1;
            count = static_cast<nrOfLightsType>(width_) * height_ * depth_;
            reduced = true;
            std::printf("  DEGRADE  buffer too large, reducing to %dx%dx%d\n",
                        static_cast<int>(width_), static_cast<int>(height_), static_cast<int>(depth_));
            if (width_ <= 8 && height_ <= 8) break; // minimum
        }
        if (!buffer_.allocate(count, channelsPerLight_)) {
            std::printf("  DEGRADE  buffer_.allocate failed at minimum size %u\n",
                        static_cast<unsigned>(count));
            setStatus("buffer allocation failed — not enough memory", Severity::Error);
        } else if (reduced) {
            setStatus("buffer reduced — not enough memory", Severity::Warning);
        }
        setDynamicBytes(buffer_.bytes() + lut_.memoryUsed());
    }
};

// EffectBase accessor implementations
inline Layer* EffectBase::layer() const { return static_cast<Layer*>(parent()); }
inline uint8_t* EffectBase::buffer() { return layer()->buffer().data(); }
inline lengthType EffectBase::width() const { return layer()->width(); }
inline lengthType EffectBase::height() const { return layer()->height(); }
inline lengthType EffectBase::depth() const { return layer()->depth(); }
inline uint8_t EffectBase::channelsPerLight() const { return layer()->channelsPerLight(); }

/// Write one non-color channel of light `index`. Silently does nothing when the fixture has no
/// such channel (offset absent) or the write would fall outside the light, which is what makes a
/// moving-head effect harmless on an LED strip. Never scaled by brightness: see architecture.md.
inline void effectSetChannel(Layer* l, nrOfLightsType index, uint8_t offset, uint8_t value) {
    if (offset == FixtureChannels::kAbsent || !l) return;
    const uint8_t cpl = l->channelsPerLight();
    if (offset >= cpl) return;
    Buffer& b = l->buffer();
    if (index >= b.count() || !b.data()) return;
    b.data()[static_cast<size_t>(index) * cpl + offset] = value;
}

inline void EffectBase::setPan(nrOfLightsType index, uint8_t value) {
    effectSetChannel(layer(), index, layer()->fixtureChannels().pan, value);
}
inline void EffectBase::setTilt(nrOfLightsType index, uint8_t value) {
    effectSetChannel(layer(), index, layer()->fixtureChannels().tilt, value);
}
inline void EffectBase::setZoom(nrOfLightsType index, uint8_t value) {
    effectSetChannel(layer(), index, layer()->fixtureChannels().zoom, value);
}
inline void EffectBase::setRotate(nrOfLightsType index, uint8_t value) {
    effectSetChannel(layer(), index, layer()->fixtureChannels().rotate, value);
}
inline void EffectBase::setGobo(nrOfLightsType index, uint8_t value) {
    effectSetChannel(layer(), index, layer()->fixtureChannels().gobo, value);
}
inline bool EffectBase::movable() const { return layer()->fixtureChannels().movable(); }
inline bool EffectBase::hasBeam() const {
    // Null-checked, unlike the accessors above, because this one is called from defineControls():
    // /api/types and /api/modules build a PARENTLESS probe instance to read an effect's control set
    // (ModuleFactory::registerType), and a bare layer() there dereferences null. An unparented
    // effect has no fixture, so it has no beam.
    const Layer* l = layer();
    if (!l) return false;
    const FixtureChannels& fc = l->fixtureChannels();
    return fc.gobo != FixtureChannels::kAbsent || fc.rotate != FixtureChannels::kAbsent;
}
inline nrOfLightsType EffectBase::nrOfLights() const { return layer()->buffer().count(); }
inline uint32_t EffectBase::elapsed() const { return layer()->elapsed(); }
inline draw::Canvas EffectBase::canvas() {
    Layer* l = layer();
    return draw::Canvas::of(l->buffer(), l->width(), l->height(), l->depth());
}

} // namespace mm
