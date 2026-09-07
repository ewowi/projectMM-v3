#pragma once

#include "light/effects/EffectBase.h"
#include "core/moonlive/MoonLive.h"
#include "light/moonlive/MoonLiveScript.h"
#include "light/moonlive/MoonLiveParticles.h"
#include "light/moonlive/MoonLiveBuiltins_light.h"
#include <cstring>
#include <cstdio>

// MoonLiveEffect: a scripted effect rendered by the MoonLive engine (§3.3 of
// livescripts-analysis-top-down.md). The thin binding side of the engine/binding seam: it
// IS a first-class EffectBase (role, controls, lifecycle, generic UI), and its tick()
// delegates to a compiled MoonLive over this effect's own buffer.
//
// The effect holds a `source` text control; prepare compiles it through the engine and
// tick() runs the emitted native code over the buffer (emit → allocExec → call → write). A
// source edit recompiles live; a parse error shows in the module status and the layer goes
// dark: robust, no reboot.

namespace mm {

/// Effect whose render is a live-authored MoonLive script.
class MoonLiveEffect : public EffectBase {
public:
    /// Both answered by the SCRIPT when it says, and by the binding when it stays silent.
    ///
    /// 📝 marks a script that declared no tags of its own: the notepad says "this is scripted",
    /// which is all a module can say about a program it has not been told about. A script that
    /// declares `string tags()` replaces it, so its row reads like any other effect's.
    const char* tags() const override {
        const char* t = script_.tags();
        return t ? t : "📝";
    }

    /// The layer EXTRUDES on this (Layer::tick), so a script declaring 1 paints one column and the
    /// framework fills the rest, exactly as a compiled D1 effect does. A script that declares
    /// nothing stays D2, which is what every script rendered as before it could say.
    Dim dimensions() const override { return script_.dimensions(); }

    // The effect carries its script's NAME as an editable, persisted text control, plus a control
    // for every control the script declared (`addControl("speed", speed, 0, 99)`). The
    // engine exposes the declared list after a compile; each becomes a real uint8 control bound by
    // reference to the engine's live control-arena slot, so a slider write lands in the slot the
    // next render tick reads, with no recompile (the live-edit guarantee). Naming a different
    // script recompiles (the script-editor loop), which re-derives the control set.
    void defineControls() override {
        // The script NAME, not the script. The text lives in a file the UI loads, edits and
        // saves through /api/file: so a module costs ~32 bytes here instead of a resident
        // kilobyte, and a script is bounded by the filesystem rather than by this array.
        controls_.addFilePath("script", script_.buffer(), script_.bufferSize(),
                              moonlive::kEffectPick);
        // Every control the script declared. System variables (`width`, `height`, `depth`, `t`)
        // are not controls and never appear here, so there is nothing to filter out.
        script_.publishDeclaredControls(controls_);
    }

    // Naming a different script must recompile: route it through the prepare rebuild sweep so the
    // new one swaps in live (the script-editor loop). A SCRIPTED CONTROL's value change must NOT
    // recompile: it just updates an arena byte the running native code reads next tick. So only
    // "script" triggers a rebuild; every scripted control returns false (the live-edit path).
    bool affectsPrepare(const char* controlName) const override {
        return std::strcmp(controlName, "script") == 0;
    }

    // Compile the source on the cold rebuild path. A failed compile (parse error or no exec
    // memory) surfaces in the module status and leaves tick() a no-op: the effect renders
    // dark, the device keeps running (robustness + no-reboot). A *source* edit re-enters here and
    // recompiles, so a new script swaps in live; a broken edit just shows its diagnostic.
    // Compile the script if the file changed, then surface whatever it declares.
    //
    // sync() answers "is what is compiled still what the file says" from a 4-byte hash, so an
    // unchanged script costs a read rather than a re-JIT. It reports the status and the dynamic
    // bytes itself, which is why nothing here repeats that.
    void prepare() override {
        // The next tick is the first: it has no previous frame to measure against. Without this the
        // frame after a disable, a re-enable or a resize hands the flow and the decay the whole
        // interval the module sat idle, which teleports the trail and fades it away. Trails, Nebula
        // and Fluid reset the same flag in prepare() for the same reason; setting it once at
        // construction covers only the very first frame of the module's life.
        tickStarted_ = false;
        // The script sizes its own pool from defineControls(), which sync() runs after a compile.
        script_.setPoolSizer([](void* ctx, uint16_t n) -> uint16_t {
            return static_cast<MoonLiveEffect*>(ctx)->particles_.resize(n);
        }, this);
        // Whether the script wants a trail plane, asked the same way and for the same reason: two
        // 16-bit planes are 96 KB on a 20-cube, so only a script that advects pays for them.
        script_.setTrailSizer([](void* ctx, bool want) -> bool {
            return static_cast<MoonLiveEffect*>(ctx)->resizeTrail(want);
        }, this);
        script_.sync(moonlive::effectSysVars(), *this);
        // The planes follow the FIXTURE, so a resize re-sizes them even though the script did not
        // change: sync() only re-runs defineControls when the source did.
        if (trailWanted_) resizeTrail(true);
        // The compile re-derives the declared-control set, so rebuild the control list to surface
        // it (the same rebuildControls() pattern NetworkModule uses when a state change reshapes
        // its controls). Each scripted control re-binds to its (stable-address) arena slot.
        // Unconditional: a control list is also rebuilt for a script that did NOT change, which
        // costs a walk and keeps the card correct after any other reason to prepare.
        rebuildControls();
    }

    void tick() MM_NONBLOCKING override {
        // The native emitter stores R,G,B at offsets +0/+1/+2 with channelsPerLight() only as the
        // stride (moonlive_lower_*: addr = index * cpl, then 3 writes). A 0/1/2-channel layer would
        // let the last light's +1/+2 write run past the buffer, so a sub-RGB layout renders dark.
        const auto cpl = channelsPerLight();
        if (cpl < 3) return;
        if (!script_.ok()) return;
        // Refresh the system variables before the script runs: a layer can be resized live, and a
        // script holding last frame's width would draw to the old geometry.
        writeSysVar(moonlive::kSysWidth,  width());
        writeSysVar(moonlive::kSysHeight, height());
        writeSysVar(moonlive::kSysDepth,  depth());
        // The draw builtins (line) render through the same canvas every native effect uses,
        // installed for exactly one run and detached after, so a script can only ever draw into
        // the layer it is ticking in.
        moonlive::setDrawCanvas(canvas());
        // fade(amt) asks the LAYER, which collects the request and applies it once per frame.
        // Installed in the same bracket as the canvas so it detaches on the same path.
        moonlive::setFadeSink([](void* ctx, uint8_t amt) {
            if (Layer* l = static_cast<MoonLiveEffect*>(ctx)->layer()) l->fadeToBlackBy(amt);
        }, this);
        // The role setters reach the fixture's motion channels, whose offsets live in the layer's
        // channel map. Routed through EffectBase's own setters, so a script aims a head by exactly
        // the path a compiled effect does, including the no-op on a light that has no such channel.
        moonlive::setMotionSink([](void* ctx, moonlive::MotionAxis axis, uint32_t index,
                                   uint8_t value) {
            auto* self = static_cast<MoonLiveEffect*>(ctx);
            const auto i = static_cast<nrOfLightsType>(index);
            switch (axis) {
                case moonlive::MotionAxis::Pan:    self->setPan(i, value);    break;
                case moonlive::MotionAxis::Tilt:   self->setTilt(i, value);   break;
                case moonlive::MotionAxis::Zoom:   self->setZoom(i, value);   break;
                case moonlive::MotionAxis::Rotate: self->setRotate(i, value); break;
                case moonlive::MotionAxis::Gobo:   self->setGobo(i, value);   break;
            }
        }, this);
        // The particle builtins reach this effect's own pool, with the frame scale the binding
        // computed: framerate independence is the system's property, not the script author's.
        if (particles_.count() > 0)
            moonlive::setPoolSink(&particles_.pool(), particles_.advance(elapsed()));
        // The trail plane, for a script that advects. Allocated only when one asked for it, since
        // it is two 16-bit planes: on a 20-cube that is 96 KB, which a script drawing dots must not
        // pay. The binding owns the geometry, the ping-pong and the dt for the same reason it owns
        // the particle pool's frame scale: none of it is the script author's to get right.
        const uint32_t nowMs = elapsed();
        // A zero delta on the first tick, and on the first after a disable: `nowMs - 0` hands the
        // flow and the decay the whole uptime, which teleports the trail and fades it away on the
        // frame it starts. Trails, Nebula and Fluid each carry this guard; the binding needs it for
        // the same reason.
        const uint32_t dt = tickStarted_ ? nowMs - lastTickMs_ : 0u;
        lastTickMs_ = nowMs;
        tickStarted_ = true;
        if (trailA_) {
            moonlive::FlowSink f{};
            f.a = trailA_.data();
            f.b = trailB_.data();
            f.front = &trailFront_;
            f.frame = &frameCount_;
            f.w = width(); f.h = height(); f.d = depth();
            f.dtMs = dt;
            moonlive::setFlowSink(f);
        }
        // The frame moment: run `tick` if the script defined one. A script that defines only
        // `modifyLogical` renders nothing here and folds coordinates instead, which is the author's
        // choice rather than an error.
        if (script_.engine().hasEntry(moonlive::kEntryTick))
            script_.engine().run(buffer(), nrOfLights(), cpl, elapsed(), moonlive::kEntryTick);
        // The trail plane onto the layer, taking each channel's high byte: the one narrowing step,
        // and the one the script cannot do for itself. Without it a script advects and decays a
        // plane nobody ever reads, which renders black however correct the flow is.
        if (trailA_) {
            moonlive::setFlowSink({});
            blitTrail();
        }
        // Outside the trail branch: the counter is the BINDING's frame count, and `fieldRate(n)` is
        // a rate limiter any script may use. Advanced only when a trail existed, a script that
        // skips `trail(1)` got a frozen counter and fieldRate answered 1 on every frame forever.
        frameCount_++;
        moonlive::setPoolSink(nullptr, 0);
        moonlive::setMotionSink(nullptr, nullptr);
        moonlive::setFadeSink(nullptr, nullptr);
        moonlive::setDrawCanvas({});
    }

    void release() override {
        particles_.release();      // zero the pool BEFORE the base frees its buffers, or it would
                                   // be left naming freed memory
        script_.engine().free();   // release the exec block: the destructor role
        script_.invalidate();     // and forget what was compiled, so re-enabling rebuilds it
        script_.releaseReporting(*this);
        // Forget the trail's shape as well as its memory: EffectBase frees the buffers, but a
        // stale trailW_/H_/D_ would make the next prepare() compare the new geometry against a
        // shape whose planes no longer exist, and skip the clear it should have done.
        releaseTrail();
        EffectBase::release();
    }

    /// Replace the script. The next prepare() compiles it: the same path a UI edit takes, so a
    /// test and a user exercise identical code.
    /// Point the module at a script in the shared script directory. The file itself is written by
    /// the UI (or the File Manager); this only says WHICH one, and the next prepare() compiles it.
    void setScript(const char* name) { script_.setName(name); }

private:
    // Publish one system variable into its arena slot, FULL WIDTH. It used to saturate to a byte,
    // which is what made a 768-wide wall report 255 and every 2D script paint a corner.
    void writeSysVar(uint8_t offset, uint32_t value) {
        moonlive::writeSysVarSlot(script_.engine().controlSlot(offset), value);
    }


    // The script this effect renders: its file name, the compiled program, and the content hash
    // that decides whether a prepare has anything to do. A fresh card starts with NO script and
    // renders nothing until one is named, rather than every new module compiling the same effect.
    moonlive::MoonLiveScript script_;
    moonlive::MoonLiveParticles particles_{*this};
    // The trail planes: two, because advection reads one and writes the other (reading and writing
    // one buffer would sample pixels the same pass had already moved). `trailFront_` says which
    // holds the trail, and the flow builtins flip it through a pointer.
    ScratchBuffer<uint16_t> trailA_{*this};
    ScratchBuffer<uint16_t> trailB_{*this};
    ScratchBuffer<uint8_t>  trailCarry_{*this};   ///< the dither's per-channel error
    bool                    trailFront_ = true;
    uint32_t                frameCount_ = 0;   ///< the counter fieldRate reads
    bool                    trailWanted_ = false;
    uint32_t                lastTickMs_ = 0;
    bool                    tickStarted_ = false;   ///< has a frame been timed yet
    lengthType              trailW_ = 0, trailH_ = 0, trailD_ = 0;  ///< the shape the planes hold

    /// The trail plane onto the layer. Through `draw::blit16` like the compiled effects: a
    /// fourth hand-rolled copy of the narrowing loop is what left this one truncating where the
    /// others dithered, which is the divergence blit16 exists to end.
    void blitTrail() {
        const lengthType w = width(), h = height(), d = depth();
        if (!trailA_ || !trailB_) return;
        const uint16_t* live = trailFront_ ? trailA_.data() : trailB_.data();
        draw::blit16(canvas(), live, w, h, d, trailCarry_ ? trailCarry_.data() : nullptr);
    }

    /// Size (or free) the trail planes for the current fixture. Returns whether one is available,
    /// which is what the script sees: a device too small to hold them reports the truth rather than
    /// rendering nothing in silence.
    bool resizeTrail(bool want) {
        trailWanted_ = want;
        // Every exit frees all THREE buffers together: the carry is as much part of the trail as
        // the planes, and leaving it behind on a failure path is memory nothing will ever read.
        if (!want) { releaseTrail(); return false; }
        const lengthType w = width(), h = height(), d = depth();
        const size_t n = static_cast<size_t>(w) * h * d * 3;
        if (n == 0) { releaseTrail(); return false; }
        const size_t had = trailA_.count();
        if (!trailA_.resize(n) || !trailB_.resize(n) || !trailCarry_.resize(n)) {
            releaseTrail();                             // a half-allocated set is worse than none
            return false;
        }
        // Same sample count but a different shape (8x16 to 16x8, or a cube reshaped): resize() kept
        // the samples and they are laid out for the old geometry, so they would smear. BOTH planes,
        // because the ping-pong swaps the spare one in on the very next frame. The guard Trails,
        // Nebula and Fluid all carry; a scripted trail is no different.
        if (n == had && (w != trailW_ || h != trailH_ || d != trailD_)) {
            std::memset(trailA_.data(), 0, trailA_.bytes());
            std::memset(trailB_.data(), 0, trailB_.bytes());
            std::memset(trailCarry_.data(), 0, trailCarry_.bytes());   // per light, so it reshapes too
        }
        trailW_ = w; trailH_ = h; trailD_ = d;
        return true;
    }

    /// Free the trail set. One place, so a new buffer cannot be forgotten on one exit path.
    void releaseTrail() {
        trailA_.resize(0); trailB_.resize(0); trailCarry_.resize(0);
        trailW_ = trailH_ = trailD_ = 0;
    }
};

}  // namespace mm
