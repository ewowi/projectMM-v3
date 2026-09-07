#pragma once

#include <cstdint>
#include <cstddef>
#include <cstdio>   // the builtin-table overflow diagnostic

// MoonLive built-in table: the neutral seam by which a HOST registers the functions a script
// may call (the ESPLiveScript `arti_external_function` / ARTI / doc §3.4 model). The core
// compiler knows only *that a name maps to a descriptor*; it owns no function names and no
// domain semantics. The light domain (or any other host) populates the table with its own
// vocabulary: setRGB/fill/random16 for LEDs, something else for a display or a sensor.
//
// A descriptor says how a call lowers:
//   - Call  : a pure host helper: lower to a generic call to `fn` (a C function pointer),
//              one argument in, one result out. (random16, later sin/cos/hsvToRgb…)
//   - Inline: a routine the backend emits inline (no per-call overhead: the hot-path
//              writers): the descriptor carries an `inlineOp` TAG, a neutral opcode the
//              per-ISA lowering knows how to emit. The core never interprets the tag; it just
//              threads it through. The light domain decides which names map to which tags.
//
// This is what keeps the core domain-neutral while the hot path stays inline: the *name*
// "setRGB" and its RGB meaning live only in the host's registration; the core sees a tag.

namespace mm::moonlive {

// A script member's TYPE. A semantic, not a storage width: every SCALAR occupies one uniform
// 4-byte slot whatever its type, and only ARRAYS pack by element. That is what removes the width
// machinery a script used to spell for itself (uint8_t/uint16_t/int16_t), which is where four
// bugs came from: a wrapped member, a sentinel read through a 16-bit window, a one-byte store
// into a two-byte member, a sign-blind array load. Here rather than with the IR because a builtin
// descriptor names the type its by-reference argument takes.
//
// Byte and Bool are masked and normalized on STORE, so a slot always already holds what its type
// promises and every read is one plain 32-bit load. Fixed is Q16.16 on that same slot. Str holds
// an offset into the compiled program's string pool.
enum class CtrlType : uint8_t { Int, Byte, Bool, Fixed, Str };

/// Bytes ONE ELEMENT occupies. A scalar always takes a whole 4-byte slot (see ctrlSlotBytes);
/// this is the array element width, which is where packing still pays for itself: a byte[] heat
/// map costs a quarter of what an int[] would, and on the classic ESP32 there is no PSRAM to
/// absorb the difference.
constexpr uint8_t ctrlWidth(CtrlType t) {
    return (t == CtrlType::Byte || t == CtrlType::Bool) ? 1 : 4;
}

/// Bytes a SCALAR of this type occupies: always 4, whatever the type. Spelled as a function
/// rather than a bare constant so the uniformity is stated at every call site that used to ask
/// for a width.
constexpr uint8_t ctrlSlotBytes(CtrlType) { return 4; }



// Neutral inline opcodes: "store shapes a backend can emit", not "LED operations". A host maps
// its function names onto these; a backend implements them. StoreElem = store N bytes (one
// element) at a computed index; FillElems = a counted loop writing one element per slot. The
// core treats them as opaque tags; the per-ISA backend and the host both know the element is 3
// bytes (RGB) for the light host, but that meaning lives outside the core.
enum class InlineOp : uint8_t {
    StoreElem,   // operands: indexVReg, v0, v1, v2  → store three values at `index`
    StoreFirst,  // operands: v0, v1, v2             → store three values at element 0
    FillElems,   // operands: v0, v1, v2             → loop store over every element
};

enum class BuiltinKind : uint8_t { Call, Inline };

// A host callable. THREE unsigned args in, one unsigned result out.
//
// One argument covered a unary helper like random16, but a binding that hands the host a POSITION
// A host function receives a POINTER to its arguments, not the arguments themselves.
//
// The compiler already evaluates every argument into a CONSECUTIVE frame slot (the stack machine's
// argument staging), so the call only has to say where they start. Each backend materialises that
// address from its own frame pointer: the arithmetic spillStore/spillLoad already do: which means
// the number of arguments is bounded by frame slots rather than by how many the calling convention
// can carry. `draw::line` takes seven; a fixed three would have forced it to be split into bespoke
// halves, and every power function added after it would inherit the same distortion.
//
// Deliberately still THREE C parameters, so each assembler's call sequence is untouched. On Xtensa
// that sequence is a windowed `call8`, the most defect-prone code in this project; widening it would
// have spent the change there and still left a fixed maximum, just a larger one.
//
// `arena` is the control/system-variable block, as before.
// `args` points at `argc` frame slots. The element type is uintptr_t because a frame slot IS one
// machine word: 8 bytes on arm64, 4 on Xtensa and RISC-V: and the backends store a whole word per
// slot. Reading them as uint32_t made args[1] land on the upper half of slot 0 on a 64-bit host,
// which is a value of 0 rather than the argument: correct on both devices, wrong on the desktop.
using HostCallFn = uint32_t (*)(const uintptr_t* args, uint32_t argc, const uint8_t* arena);

struct Builtin {
    const char*  name = nullptr;      // the script-visible name (host-owned)
    uint8_t      argc = 0;            // number of arguments
    bool         returns = false;     // Call: produces a value (an expression) vs a void statement
    BuiltinKind  kind = BuiltinKind::Call;
    HostCallFn   fn = nullptr;        // Call: the host C function pointer
    InlineOp     inlineOp{};          // Inline: the neutral opcode tag
    // Which arguments are passed BY REFERENCE, as a bit per position (bit 0 = first argument).
    // A script names a member and the compiler passes its arena offset, so `addControl("bpm", bpm,
    // 1, 120)` reads as the reference a compiled module passes rather than as bpm's value. Zero
    // for every builtin that takes plain values, which is all of them but this one.
    //
    // A bitmask rather than a per-argument enum because the only question is by-value or
    // by-reference, and `draw::line` already proves a builtin may take seven arguments.
    uint8_t      byRef = 0;
    // Which arguments must be a STRING LITERAL, a bit per position. Without it a bare identifier
    // in a name slot compiles: `addControl(s, s, 0, 9)` read `s`'s VALUE as the label and handed the
    // host a pointer built from a color byte. Stated per builtin for the same reason byRef is,
    // rather than special-cased by name in the parser.
    uint8_t      byStr = 0;
    // Which arguments are FIXED (Q16.16) rather than whole numbers, a bit per position, and
    // whether the RESULT is. Stated per builtin for the same reason byRef and byStr are: the
    // parser type-checks against this rather than matching on a name, so the next builtin that
    // speaks fixed declares it here and the checker follows.
    //
    // Almost every builtin is whole numbers: a channel, a light index, an angle16, a count. The
    // exceptions are the ones a shader hands coordinates to: uvX/uvY return a fixed coordinate,
    // and escape() takes four of them.
    uint8_t      fixedArgs = 0;
    bool         fixedReturn = false;
};

/// Assert a host's builtin table did not silently drop a registration.
///
/// `BuiltinTable::add()` returns false when the table is full, and a host registers dozens of
/// names in a row without checking each one: so an overflow used to surface as a script failing
/// with "unknown function" for a builtin that plainly exists in the source. This turns it into a
/// failure at the point of registration. A host calls it once, after building its table.
#define MM_ASSERT_NO_BUILTIN_OVERFLOW(t)                                              \
    do {                                                                              \
        if ((t).full()) {                                                             \
            std::printf("MoonLive: builtin table FULL at %u entries, a registration " \
                        "was dropped. Raise BuiltinTable::kMax.\n",                    \
                        static_cast<unsigned>((t).registered()));                     \
        }                                                                             \
    } while (0)

// A fixed-capacity table the host fills and the compiler reads. No heap; a host registers a
// handful of functions. Lookup is by name (linear: the table is tiny).
struct BuiltinTable {
    // 96, not 16 and no longer 64. The light domain filled all 16, and a table at capacity fails
    // SILENTLY: add() returned false, no caller checked it, and the script found out as "unknown
    // function" at compile time with nothing pointing at the real cause. 64 was reached at 61 of 64
    // when the flow builtins arrived (2026-09-04), which is too little headroom for a table that
    // fails this way; the cost is `sizeof(Builtin)` per unused slot in a table the host builds once.
    static constexpr uint8_t kMax = 96;
    Builtin items[kMax];
    uint8_t count = 0;
    bool overflowed = false;    // set when an add() was DROPPED: see full()

    bool add(const Builtin& b) {
        if (b.name == nullptr) return false;   // a null name would null-deref in find()
        if (count >= kMax) { overflowed = true; return false; }
        items[count++] = b;
        return true;
    }

    /// True when a registration was dropped for lack of room. A host builds its table once at
    /// startup, so this is asserted there rather than checked per call: the point is that
    /// running out of table is LOUD, which is exactly what the 16-entry version was not.
    bool full() const { return overflowed; }

    /// Every registered name, for the overflow diagnostic. Not used on any hot path.
    uint8_t registered() const { return count; }
    const Builtin* find(const char* name, size_t len) const {
        for (uint8_t i = 0; i < count; i++) {
            const char* n = items[i].name;
            size_t j = 0;
            for (; j < len && n[j]; j++) if (n[j] != name[j]) break;
            if (j == len && n[j] == 0) return &items[i];
        }
        return nullptr;
    }
};

/// BYTES the script's own members may occupy, and separately how many members it may declare.
///
/// These were one number while every member was a byte and its offset WAS its declaration index.
/// A `uint16_t` member costs two bytes and an array costs its length, so the two stopped being the
/// same question: a script may want six members costing sixteen bytes, or two members costing
/// twelve. The byte budget is what the arena allocates; the count is what the fixed record tables
/// hold. Both are fixed, so neither needs a heap.
// 64 was chosen against the first effect that wanted an array rather than in the abstract: a
// 16-element heat buffer with two byte controls and a uint16_t phase needs 20, and a per-light
// buffer for a small fixture wants more. 64 holds a uint8_t[64] or a uint16_t[32] alongside a few
// scalars, costs 48 bytes per engine over the old value (three engines per pipeline, so 144 on a
// device), and keeps the whole arena inside a byte offset, which is what LoadCtrl's immediate and
// the DeclaredControl record both carry. Raise it against a script that needs more, not on
// speculation: the failure is a clear compile error naming the arena, so hitting it is visible.
static constexpr uint8_t kCtrlBytes = 64;        // arena bytes the script's members share
static constexpr uint8_t kMaxCtrls  = 8;         // records: how many members/controls may exist

// The controls arena holds three kinds of byte, in one allocation with a fixed split:
//   [0 .. kCtrlBytes)                 script-declared members, offset == a BYTE CURSOR assigned
//                                     in declaration order (NOT the declaration index: a member
//                                     wider than a byte, or an array, consumes several)
//   [kCtrlBytes .. kCtrlBytes+kMaxSysVars)  host system variables (width/height/…), offset assigned
//                                     by the host and CONSTANT for the program's life
//   [kDepthSlot]                      the recursion depth counter, owned by the emitted code
// System variables sit ABOVE the script's range so that adding or removing a control: which
// renumbers every control offset: cannot move them. The binding caches their slot pointers, so a
// moving offset would silently write the wrong byte.
// The emitted-code buffer is sized to THE SCRIPT (codeCapFor below), not to a constant, for the
// same reason the IR op array is: the backends differ by up to 1.9x on identical source: RISC-V is
// fixed-4-byte and saves the whole register pool around every call where Xtensa has 3-byte narrow
// forms: so any single number is either too small for the sparsest backend or wasteful for the
// densest. A fixed 2 KB let `plasma.mle` run on an S3 and desktop and REFUSED it on an S31 by 96
// bytes, which is the second time one constant made a script's portability depend on its ISA.
//
// kCodeCap survives as the SANITY bound only: a runaway script fails with a diagnostic instead of
// exhausting the heap. It is not the working limit, so it is sized well above any real script.
static constexpr size_t  kCodeCap = 16384;

/// Bytes to reserve for a script of `tokens` tokens. Over-estimating costs one cold-path allocation
/// that is freed when the compile ends; under-estimating fails a script that would have fit, so the
/// direction of the error is deliberate: the same rule the IR's op estimate follows.
///
/// 48 bytes/token, measured across every shipped script on all three backends with `countTokens`
/// (which skips comments, so a long header does not inflate the count). The densest is
/// `random-pixel.mle` at 28.5 on RISC-V: one statement, four nested `random16()` calls, and each
/// call saves and restores the whole register pool. So this is a ~1.7x margin over the worst real
/// case.
///
/// A SHORT call-dense script sets the bound, not a long one. A call lowers to a save/restore while
/// declarations and operators lower to a few instructions each, so bytes-per-token FALLS as a
/// script grows: `gradient.mle` is 5.9 where `random-pixel.mle` is 28.5, and the longest shipped
/// script (`ripples.mle`, 280 tokens) is only 15.3. The margin is kept wide for that reason rather
/// than trimmed to the observed worst: a new short call-dense script could beat 28.5, while a long
/// one cannot.
///
/// It was 64, from a measurement taken before host arguments moved into frame slots, which shrank
/// what a call saves. At 64 the two longest scripts asked for more than kCodeCap and were served by
/// the clamp, which works but means a script's buffer stopped tracking its size. Re-measuring took
/// 25% off the transient allocation, which matters on a classic ESP32 where the compile shares a
/// 12 KB task.
///
/// The floor covers a tiny script's fixed prologue and epilogue, which no per-token figure
/// expresses.
constexpr size_t codeCapFor(uint32_t tokens) {
    const size_t want = size_t(tokens) * 48 + 256;
    return want > kCodeCap ? kCodeCap : want;
}

static constexpr uint8_t kMaxSysVars  = 8;

/// Bytes per system variable. FOUR, not one: `width` on a 768-wide wall does not fit in a byte, and
/// clamping it made every script that loops `for (x = 0; x < width; …)` draw a complete picture into
/// a 255x255 corner and leave the rest of the rig black. A script scalar is already 4 bytes
/// (LoadCtrl32 exists for members), so widening these costs arena space and nothing else.
///
/// The block starts at kCtrlBytes, which is 4-byte aligned, so every slot is too: a 32-bit load
/// needs that, and one-byte spacing would have left every second slot misaligned.
static constexpr uint8_t kSysVarBytes = 4;

/// Where the emitted code keeps its RECURSION DEPTH, one byte in the arena above the system
/// variables. In the arena rather than in a C++ member because the counter is read and written by
/// the emitted block itself: a recursive call happens entirely inside the exec block, with no C++
/// frame between the activations for a host-side counter to sit in. Every function already holds
/// the arena pointer (kArg4), so the guard costs a byte and no new argument.
///
/// The host zeroes it before each run rather than trusting the block to unwind cleanly: a script
/// that hits the limit leaves the counter wherever the skipped call left it, and a stale value
/// would shrink the budget of every later frame until nothing ran at all.
static constexpr uint8_t kDepthSlot = kCtrlBytes + kMaxSysVars * kSysVarBytes;

/// The depth at which a call is REFUSED: an activation that would make the counter reach this
/// number returns without running, so 31 activations execute, the entry function included.
///
/// A fixed render-task stack makes unbounded recursion a device reset, which the robustness rule
/// forbids, so the depth is bounded at run time rather than at compile time: whether a recursion
/// terminates is not decidable from the source. The number is measured rather than chosen. An
/// activation costs 176 bytes of stack on Xtensa (48 host-call area + 84 slots + 32 window
/// reserve + alignment) against a 12 KB main task, so the device resets at roughly 64 deep. This
/// leaves the deepest legal recursion at under half the budget, which is the margin the interrupt
/// stack and the rest of the render path need.
static constexpr uint8_t kMaxCallDepth = 32;

/// WHERE A SCRIPT CALL'S ARGUMENTS ARE PASSED, and why they are not passed in the frame.
///
/// Each function opens its OWN frame (`a.prologue`), and a frame slot is addressed off the current
/// frame pointer: on Xtensa through the windowed ABI's `entry a1, N`. So a caller physically cannot
/// store into the slot the callee will read, and the obvious design (stage into the callee's
/// locals) is not expressible on the target that matters most.
///
/// The ARENA is what both activations share: every function already holds its pointer (kArg4), so
/// an argument block here costs no new register, no change to any call sequence, and in particular
/// leaves Xtensa's `call8` untouched, which is the most defect-prone code in this project.
///
/// ONE block, not one per depth, because the CALLEE COPIES ITS ARGUMENTS INTO ITS OWN FRAME as its
/// first act. The block is therefore live only across the call instruction itself, and the two
/// cases that look dangerous both hold: in `f(g(x))` the inner call has returned (and its block is
/// dead) before the outer one writes; and in recursion the callee has already copied out before it
/// writes the block for the call it makes. Copy-at-entry is what buys the single block, so it is a
/// requirement of this design rather than an optimization of it.
static constexpr uint8_t kMaxScriptArgs  = 4;     // per call; a helper wanting more wants an array
static constexpr uint8_t kScriptArgBytes = 4;     // one machine word, as a frame slot is
/// ALIGNED to a 4-byte boundary, because these are 32-bit slots: kDepthSlot is a single byte, so
/// the next free address is odd, and a word store there is unaligned. Xtensa faults on that, and a
/// misaligned word is a silently wrong value everywhere else.
static constexpr uint8_t kScriptArgBase  = static_cast<uint8_t>((kDepthSlot + 1 + 3) & ~3);

/// The arena byte offset of argument `i`.
constexpr uint8_t scriptArgOffset(uint8_t i) {
    return static_cast<uint8_t>(kScriptArgBase + i * kScriptArgBytes);
}

/// Sized from the argument block's own END, not from a sum of the parts: the block is ALIGNED up
/// from kDepthSlot, so adding the pieces underestimates it by the padding and the last argument
/// would sit past the arena the host allocates.
static constexpr uint8_t kArenaBytes  = kScriptArgBase + kMaxScriptArgs * kScriptArgBytes;

/// A name the HOST defines and the script only reads (`width`, `height`, `depth`). Reserved: a
/// script cannot declare one, so the name means the same thing in every script (the `t` rule, one
/// construct wider). Distinct from a control: nobody sets it in the UI, and it never appears in
/// declaredControls(), so no binding has to hide it.
///
/// The common case is read from the controls arena like a control is, because the value changes per frame and
/// the emitted code must not bake it in. The difference is ownership: the BINDING owns the slot
/// and writes it (from the layer), and the compiler reserves the slot rather than the script
/// declaring it.
enum class SysVarKind : uint8_t {
    Arena,   // a byte in the controls arena the binding writes per frame (width/height/depth)
    Arg,     // an argument register the host passes on every run (t): costs no instruction
};

struct SysVar {
    const char* name = nullptr;
    SysVarKind  kind = SysVarKind::Arena;
    uint8_t     where = 0;   // Arena: byte offset into the arena. Arg: the VReg (kArg0..kArg4).
};

/// The system variables one host domain defines. Same shape and lookup as BuiltinTable: a host
/// hands the compiler both, and the compiler resolves names against them without knowing the domain.
struct SysVarTable {
    // Bounded by the arena's system range, not chosen independently: a host that could register
    // more system variables than the arena reserves would hand out an offset controlSlot() rejects,
    // and the binding's per-frame write would be silently dropped.
    static constexpr uint8_t kMax = kMaxSysVars;
    SysVar  items[kMax];
    uint8_t count = 0;

    // Rejects an offset the arena cannot hold, rather than storing it and failing at run time:
    // controlSlot() would return nullptr for it and the binding's per-frame write would vanish
    // with no error anywhere. An Arena slot must sit in the system range (above the script's
    // controls, inside the arena); an Arg must name a real argument register.
    bool add(const SysVar& v) {
        if (count >= kMax || v.name == nullptr) return false;
        // Below kDepthSlot, not merely inside the arena: the depth counter sits above the system
        // range and a 4-byte LoadCtrl32 at that offset would read it and run past the arena's end.
        // Aligned to kSysVarBytes for the same reason the registrations already are, so every slot
        // is a whole 32-bit cell rather than one straddling two.
        if (v.kind == SysVarKind::Arena &&
            (v.where < kCtrlBytes || v.where >= kDepthSlot ||
             (v.where - kCtrlBytes) % kSysVarBytes != 0))
            return false;
        // kArg4 is the last argument register (MoonLiveIr.h owns the enum, and includes THIS
        // header, so the bound is spelled here rather than referenced).
        if (v.kind == SysVarKind::Arg && v.where > 4) return false;
        items[count++] = v;
        return true;
    }
    const SysVar* find(const char* name, size_t len) const {
        for (uint8_t i = 0; i < count; i++) {
            const char* n = items[i].name;
            size_t j = 0;
            for (; j < len && n[j]; j++) if (n[j] != name[j]) break;
            if (j == len && n[j] == 0) return &items[i];
        }
        return nullptr;
    }
};

}  // namespace mm::moonlive
