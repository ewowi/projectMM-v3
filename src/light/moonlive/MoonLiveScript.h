#pragma once

#include "core/MoonModule.h"
#include "light/moonlive/MoonLiveBuiltins_light.h"
#include "light/moonlive/MoonLiveScriptFile.h"

#include <cstring>

namespace mm::moonlive {

/// One scripted module's script: the file it points at, the engine holding the compiled program,
/// and the 4-byte content hash that answers "is what is compiled still what the file says".
///
/// Held BY VALUE in each binding rather than inherited from. The three scripted modules derive from
/// three sibling bases under MoonModule, so a shared base would need virtual inheritance and would
/// change the object layout of every module in the system to serve three of them.
///
/// The rule this exists to state once: **if the file changed, recompile**. That is all `sync()` is.
/// The three bindings each grew their own bookkeeping around that one rule and drifted apart:
/// an effect had no content hash at all (so editing a script's text changed nothing until it was
/// renamed), a layout cleared its hash only on a name change (same gap), and only a modifier
/// re-read the file each time. Same question, one answer.
class MoonLiveScript {
public:
    /// The longest status this module reports, "<message> @<offset>" included.
    ///
    /// Sized for the longest diagnostic the compiler emits plus the suffix. At 48 the longer
    /// messages truncated, and once the offset moved into this string a truncation also cost the
    /// editor the position it marks the failing line from: the errors hardest to read were exactly
    /// the ones that lost their explanation AND their highlight. Public because a unit test pins it
    /// against the compiler's own messages, which is what stops the two drifting apart again.
    /// Sized from the WORST line the code can produce, which the status-fits test recomputes from
    /// the compiler's own diagnostics: the longest message (62), the longer of the two shadow marks
    /// with its ": " (34), and the machine-read " @<offset>" suffix (8). At 72 the suffix was what
    /// fell off, and the editor silently stopped marking the failing line.
    static constexpr size_t kMaxStatus = 112;
    /// What the status says when the user's copy is hiding a shipped one (`scriptShadowsFactory`).
    /// One constant because the status-fits test budgets its length against kMaxStatus.
    static constexpr const char* kShadowMark = "edited copy";
    /// Said instead of kShadowMark when the shipped copy has moved on since the fork: the one
    /// sentence that turns "why is my edit being used" into "the library has a newer version".
    static constexpr const char* kStaleMark = "edited copy, shipped one updated";

    /// Let a binding that owns a particle pool size it from the script's defineControls(). Null for
    /// a binding with no particles, which is every binding but the effect today.
    void setPoolSizer(PoolSizeFn fn, void* ctx) { sizePool_ = fn; poolCtx_ = ctx; }

    /// The same for a binding that owns a trail plane: the script asks with `trail(1)`, and only a
    /// script that asks pays for the two 16-bit planes.
    void setTrailSizer(TrailSizeFn fn, void* ctx) { sizeTrail_ = fn; trailCtx_ = ctx; }

    /// Re-read the file and recompile IFF its content hash moved.
    ///
    /// Returns true when a NEW program was installed. That return value is load-bearing rather than
    /// informational: a modifier turns it into "ask the Layer to rebuild its mapping", and the Layer
    /// rebuild IS applyState(), which calls prepare() again. Returning true unconditionally makes
    /// the two call each other forever, the mapping rebuilds every frame and the fixture renders
    /// nothing at all. So this is idempotent by contract: called twice with an unchanged file, the
    /// second call returns false and touches nothing.
    ///
    /// `owner` receives the status line and the dynamic-byte figure, so a binding does not repeat
    /// the reporting. `sysvars` is the one thing that genuinely differs per role (an effect is told
    /// its grid, a layout is not), which is why it is a parameter rather than a member.
    /// `builtins` is the VOCABULARY this script compiles against, alongside `sysvars`. The two
    /// travel together: a service gets gpioRead and setControl with no `width`, an effect gets the
    /// light table with the grid variables. Defaulted to the light table so the three light
    /// bindings read exactly as they did.
    bool sync(const SysVarTable& sysvars, MoonModule& owner,
              const BuiltinTable& builtins = lightBuiltins()) {
        // Cheapest question first: does the file still hash to what is loaded? This runs on every
        // prepare sweep, and a file write now triggers one, so it must not cost a compile. One read
        // answers it, against a compile's read plus parse, codegen and exec-block allocation.
        uint32_t fileHash = 0;
        const bool readable = scriptFileHash(name_, fileHash);
        if (readable && engine_.ok() && haveCompiled_ && fileHash == compiledHash_) return false;

        // A failed compile leaves haveCompiled_ false and the engine not ok(), which is
        // indistinguishable from "not compiled yet", so without this latch a layout re-reads and
        // re-compiles the file on every lightCount()/placeLights(). Each attempt is two LittleFS
        // operations (~5 ms on an S3), the pipeline asks repeatedly while sizing and walking the
        // fixture, and the retries starve the task until the 12 s watchdog RESETS THE DEVICE.
        //
        // Keyed on the name AND the content, not the name alone. As a bare flag it latched on the
        // empty script every device boots with and skipped the compile forever; keyed on the name
        // only, it would refuse to re-try a script the user just fixed in place, which is exactly
        // what editing a file on its card does.
        //
        // An UNREADABLE file (missing, empty, oversized) hashes to nothing, so it is latched on the
        // name alone: there is no content to have changed, and a hash comparison would compare 0
        // against the hash of whatever failed last and release the latch on every ask. That is the
        // watchdog case above, reached by deleting the script a card points at.
        if (compileFailed_ && std::strcmp(failedScript_, name_) == 0 &&
            (!readable ? !failedReadable_ : (failedReadable_ && fileHash == failedHash_)))
            return false;

        resetPrintBudget();
        const char* err = nullptr;
        uint32_t hash = 0;
        if (compileScriptFile(engine_, name_, builtins, sysvars, err, &hash)) {
            // Declare the controls the script asks for, the way a compiled module does: by RUNNING
            // defineControls(). Before the binding's rebuildControls(), which turns the declared
            // list into UI cards.
            runDefineControls(engine_, sizePool_, poolCtx_, sizeTrail_, trailCtx_);
            // What the script SAYS it is, read once per compile rather than per frame. Both are
            // cold-path questions the host asks about a program, the same two a compiled module
            // answers with `Dim dimensions()` and `const char* tags()`.
            readIdentity();
            // A compiled script is not an error, but it has something to say: how big it is, and
            // the one budget it is closest to using up. The card's memory figure is the ALLOCATION,
            // word-rounded, which says nothing about the program itself.
            engine_.describe(statusBuf_, sizeof(statusBuf_));
            // Name a SHADOW when there is one, on success as much as on failure: the copy that
            // compiled is the user's, and a push to the shipped one will change nothing on screen.
            if (shadowMark()) {
                const size_t n = std::strlen(statusBuf_);
                std::snprintf(statusBuf_ + n, sizeof(statusBuf_) - n, ", %s", shadowMark());
            }
            owner.setStatus(statusBuf_, MoonModule::Severity::Status);
            compileFailed_ = false;
        } else {
            // "message @<offset>": the message a user reads, and the position the editor marks.
            // One string because status IS the channel a module reports through, and a second
            // control for the number would be a field every non-scripted module carries for nothing.
            // The suffix is machine-read, so it stays a fixed shape rather than a sentence.
            if (engine_.hasErrorPos()) {
                // The shadow marker goes in FRONT: the `@<offset>` suffix is machine-read and must
                // stay last, and the offset only makes sense against the copy that was compiled.
                // Suffix FIRST, message into what is left: a message too long for the buffer
                // loses its tail, never the offset the editor parses.
                char at[12];
                std::snprintf(at, sizeof(at), " @%u", static_cast<unsigned>(engine_.errorPos()));
                const size_t room = sizeof(statusBuf_) - std::strlen(at) - 1;
                const char* mark = shadowMark();
                int n = std::snprintf(statusBuf_, room + 1, "%s%s%s",
                                      mark ? mark : "", mark ? ": " : "",
                                      err ? err : "compile failed");
                if (n < 0) n = 0;
                if (static_cast<size_t>(n) > room) n = static_cast<int>(room);
                std::snprintf(statusBuf_ + n, sizeof(statusBuf_) - static_cast<size_t>(n), "%s", at);
                owner.setStatus(statusBuf_, MoonModule::Severity::Error);
            } else {
                owner.setStatus(err, MoonModule::Severity::Error);
            }
            // Forget what the LAST script said it was. A failed compile has already interned its
            // strings into the same pool from offset zero, so a tags_ kept from the previous
            // program now points at whatever those bytes became, and the card would show it.
            dim_ = Dim::D2;
            tags_ = nullptr;
            compileFailed_ = true;
            failedHash_ = fileHash;
            failedReadable_ = readable;   // distinguishes "this text is broken" from "no file"
            std::snprintf(failedScript_, sizeof(failedScript_), "%s", name_);
        }
        // The CONTENT hash, not a copy of the text: 4 bytes to answer "is what I compiled still what
        // the file says". Whether anything IS compiled is a separate flag rather than hash != 0,
        // because 0 is a legitimate hash: a script that happened to hash to it would recompile on
        // every prepare sweep, which is the cost this comparison exists to avoid.
        compiledHash_ = hash;
        haveCompiled_ = engine_.ok();   // a FAILED compile has no program, whatever the file hashed to
        // ADD the engine's heap to whatever else the owner holds, rather than assigning it: a
        // binding may also own ScratchBuffers (a particle pool), and those report themselves
        // through the buffer's own delta hook. Assigning here would erase them, which is the
        // "don't mix addDynamicBytes with setDynamicBytes" contract MoonModule states. Tracking
        // what this script last reported keeps it a delta rather than a running total.
        const size_t nowBytes = engine_.heapBytes();
        owner.setDynamicBytes(owner.dynamicBytes() - reportedBytes_ + nowBytes);
        reportedBytes_ = nowBytes;
        return true;
    }

    /// The name buffer the `script` control binds to. A control write lands here DIRECTLY, without
    /// passing through setName(), which is why sync() re-derives from the file rather than trusting
    /// a flag someone remembered to clear.
    char*  buffer() { return name_; }
    size_t bufferSize() const { return sizeof(name_); }
    const char* name() const { return name_; }

    /// Point at a different script. The next sync() compiles it.
    void setName(const char* n) {
        if (!n) return;
        std::snprintf(name_, sizeof(name_), "%s", n);
        invalidate();
    }

    /// Hand back everything this script reported to its owner. Called from a binding's release(),
    /// The dimensionality the script declared, or D2 when it declared none.
    ///
    /// D2 is the fallback because it is what every script rendered as before scripts could say,
    /// so a script that stays silent keeps behaving exactly as it did. An out-of-range answer is
    /// treated the same way: a script cannot make the layer extrude along an axis that does not
    /// exist.
    Dim dimensions() const { return dim_; }

    /// The emoji the script declared, or nullptr when it declared none (the binding then keeps its
    /// own default). Points into the engine's string pool, which lives as long as the compiled
    /// program, so it stays valid until the next compile replaces it.
    const char* tags() const { return tags_; }

    /// AFTER engine().free(): the exec block is gone, so the owner's card must stop counting it.
    ///
    /// One home for all three bindings rather than two lines each: MoonLive::free() does not touch
    /// the owner's total, so a binding that forgets this leaves a disabled module reporting memory
    /// it no longer holds. Subtracting rather than zeroing is what lets a binding own OTHER memory
    /// too (the effect's particle pool), which a setDynamicBytes(0) would wrongly erase.
    void releaseReporting(MoonModule& owner) {
        const size_t held = owner.dynamicBytes();
        owner.setDynamicBytes(held > reportedBytes_ ? held - reportedBytes_ : 0);
        reportedBytes_ = 0;
    }

    /// Forget what is compiled, so the next sync() rebuilds. For a module coming back from
    /// disabled, where the engine was released but the name was kept.
    void invalidate() {
        compiledHash_ = 0;
        haveCompiled_ = false;
        compileFailed_ = false;
        failedHash_ = 0;
        failedReadable_ = false;
        failedScript_[0] = '\0';
    }

    /// Publish every control the compiled script declared into `controls`, bound by reference to
    /// the engine's live arena slot so a slider write lands where the running native code reads it.
    /// The ONE home for this: all three bindings (effect, layout, modifier) publish identically,
    /// and the type dispatch below is the kind of reasoning that should be stated once.
    void publishDeclaredControls(ControlList& controls) {
        uint8_t n = 0;
        const moonlive::DeclaredControl* decls = engine_.declaredControls(n);
        for (uint8_t i = 0; i < n; i++) {
            uint8_t* slot = engine_.controlSlot(decls[i].offset);
            if (!slot) continue;   // engine not compiled yet: controls appear after prepare
            // Published as the widget the member's TYPE calls for. Every scalar occupies the same
            // 4-byte slot, so this is no longer a width dispatch: it is the semantic one, and the
            // storage underneath is identical in all three cases.
            //
            // Safe to view the slot as its type: the compiler aligns every member to a 4-byte
            // arena offset, and the arena base comes from platform::alloc, which is aligned for
            // any fundamental type.
            //
            // byte and bool point at the slot's LOW BYTE, which is only correct because the two
            // are masked on store: the upper three bytes are always zero, so a 1-byte control
            // reading and writing that byte sees the member's whole value. On a big-endian target
            // the low byte would be at offset+3: no supported target is one.
            switch (decls[i].type) {
                case moonlive::CtrlType::Bool:
                    // NORMALIZED before the byte is ever read as a `bool`. A script's store
                    // truncates rather than normalizing, so a bool member can legally hold 7
                    // (`flag = 7;` is ordinary arithmetic to the language), and a C++ bool object
                    // holding anything but 0 or 1 is undefined behavior the moment it is read.
                    // One write at publish time settles it; every later write comes through
                    // applyControlValue's parseBool, which yields 0 or 1 by construction.
                    *slot = (*slot != 0) ? 1 : 0;
                    controls.addControl(decls[i].name, *reinterpret_cast<bool*>(slot));
                    break;
                case moonlive::CtrlType::Byte:
                    controls.addControl(decls[i].name, *slot,
                                      static_cast<uint8_t>(decls[i].min),
                                      static_cast<uint8_t>(decls[i].max));
                    break;
                default:   // Int; Fixed and Str never reach here (the compiler refuses to bind one)
                    controls.addControl(decls[i].name, *reinterpret_cast<int32_t*>(slot),
                                      decls[i].min, decls[i].max);
                    break;
            }
            // The member's initializer (`byte bpm = 60;`) IS the control's default, and it is
            // the only place one exists: /api/types probes a fresh module for defaults, and a
            // scripted module's controls come from the script, so a probe with no script declares
            // none. Carried on the control instead, which is what lights the UI's reset button.
            controls.setDefault(controls.count() - 1, static_cast<int32_t>(decls[i].def));
        }
    }

    MoonLive&       engine()       { return engine_; }
    const MoonLive& engine() const { return engine_; }
    bool ok() const { return engine_.ok(); }

private:
    /// Read what the script says it is, once per compile. Both questions a compiled module answers
    /// with a member function, asked here the same way: by running the function the script wrote.
    ///
    /// A script that declares neither keeps the binding's own defaults, so every script written
    /// before these existed behaves exactly as it did.
    void readIdentity() {
        dim_ = Dim::D2;
        tags_ = nullptr;
        // runValue answers only for a function whose DECLARED type matches, so a script that wrote
        // `void dimensions()` gets the fallback rather than the return register's contents.
        const uintptr_t d = engine_.runValue("dimensions", moonlive::RetType::Int, 2);
        if (d >= 1 && d <= 3) dim_ = static_cast<Dim>(d);
        const uintptr_t s = engine_.runValue("tags", moonlive::RetType::Str, 0);
        if (s) tags_ = reinterpret_cast<const char*>(s);
    }

    MoonLive engine_;
    /// What the compiled script declared, cached: both are cold-path answers the host asks once,
    /// and re-running a script function to answer them per frame would put a call on the tick path.
    Dim         dim_  = Dim::D2;
    const char* tags_ = nullptr;
    // Backing store for the status line: MoonModule::setStatus keeps a POINTER, so the text has to
    // outlive the call. The same module-owned pattern NetworkModule uses.
    /// Which shadow note this script's status should carry, or null when the file being compiled is
    /// the only copy of it. Asked on every compile (a cold path), and asked ONCE per status line so
    /// the success and failure branches cannot drift into saying different things.
    const char* shadowMark() const {
        if (!moonlive::scriptShadowsFactory(name_)) return nullptr;
        return moonlive::scriptFactoryMovedOn(name_) ? kStaleMark : kShadowMark;
    }

    char     statusBuf_[kMaxStatus] = {};
    // The script's FILE NAME, inside the shared script directory. Empty on a fresh card: it reports
    // "no script" until one is named, rather than every new module compiling the same default.
    char     name_[kMaxScriptName + 1] = "";
    uint32_t compiledHash_ = 0;
    bool     haveCompiled_ = false;   // 0 is a valid hash, so "is anything compiled" is its own flag
    // The name AND content that failed, so a retry is skipped only while both still match. Content
    // too, because the whole point of this step is that a file's text changes under a fixed name:
    // latching on the name alone would refuse to re-try a script the user just fixed.
    bool     compileFailed_ = false;
    bool     failedReadable_ = false;   // was there a file at all when it failed?
    uint32_t failedHash_ = 0;
    char     failedScript_[kMaxScriptName + 1] = "";

    size_t     reportedBytes_ = 0;   // what this script last added to the owner's total
    PoolSizeFn  sizePool_  = nullptr;
    void*       poolCtx_   = nullptr;
    TrailSizeFn sizeTrail_ = nullptr;
    void*       trailCtx_  = nullptr;
};

}  // namespace mm::moonlive
