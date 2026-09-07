#pragma once

#include <cstdint>
#include <cstddef>
#include "core/moonlive/MoonLiveBuiltins.h"   // InlineOp (a neutral opcode tag)
#include "platform/platform.h"                // alloc/free — the op array is sized to the script

// MoonLive IR — the typed intermediate representation between the front-end and the per-ISA
// assembler (§3.2 of livescripts-analysis-top-down.md). The front-end lowers an AST to a flat
// list of three-address ops over virtual registers; each per-ISA backend lowers that same IR
// to machine bytes. The IR is the seam: it knows the *operations*, never the *ISA* and never a
// domain (no LEDs). Buffer writes are not a special IR op — they are `Inline` ops carrying a
// neutral opcode tag the HOST registered (see MoonLiveBuiltins.h); the core just threads the
// tag to the backend.
//
// It is a compile-time data structure — consumed during lowering, never present at run time.
// A fixed-capacity op list (no heap in the build path); a single statement is a handful of ops.
//
// Virtual registers are plain indices v0..v(kMaxVRegs-1). A backend maps them to machine
// registers. The named host arguments arrive in fixed vregs (kArg…) so the front-end refers to
// them without knowing the ABI. They are named neutrally — kArg0..kArg4 — and a host assigns
// meaning (for the light host: buf, nLights, cpl, t, and a controls-values pointer).

namespace mm::moonlive {

using VReg = uint8_t;

// kArg4 is a per-instance data pointer the host passes at run time (for the light host: the
// control-values arena). LoadCtrl reads a byte from it — a script control whose value the binding
// updates live, without a recompile (the kArg3/t pattern, one slot over).
enum : VReg { kArg0 = 0, kArg1 = 1, kArg2 = 2, kArg3 = 3, kArg4 = 4, kFirstTemp = 5 };

// How many values one program may NAME. Deliberately larger than any target's register file: the
// register allocator (MoonLiveSpill.h) parks whatever does not fit in frame slots, so this bounds
// the compiler's own tables — the interval array is one entry per vreg, on the stack of a 12 KB task
// — rather than the script. 32 keeps that array at a few hundred bytes.
static constexpr uint8_t kMaxVRegs = 32;
// An upper SANITY bound, not the working limit: the op array is sized to the script (see IrProgram),
// so a one-statement script pays for one statement. This exists only so a runaway source fails with
// a diagnostic instead of asking for an allocation that would exhaust a small device's heap.
static constexpr uint16_t kMaxIrOps = 4096;

// Ops a single source token can lower to, worst case. The compiler sizes its op array by counting
// tokens and multiplying — an over-estimate by construction, which is the safe direction: a few
// unused entries on a cold path, versus refusing a script that would have fit.
static constexpr uint16_t kIrOpsPerToken = 4;

// The op set — neutral. Three-address form: dst plus up to three source operands. (Counted
// Control flow arrived with the script-level `for`, which is what the note here anticipated: the
// StoreElem/FillElems inline ops still carry their own loop in the per-ISA lowering, but a loop a
// SCRIPT writes cannot live inside one opcode. Three ops carry it, and they are deliberately the
// smallest set that every backend already has instructions for — a label is a position, and the
// only branch every ISA here exposes is compare-and-branch-if-greater-or-equal (arm64 spells it
// cmp + b.hs, Xtensa and RISC-V have bgeu directly).
enum class IrOp : uint8_t {
    Const,     // dst = imm
    Add,       // dst = a + b
    AddImm,    // dst = a + imm
    Mul,       // dst = a * b
    Mulhi,     // dst = the SIGNED high 32 bits of a * b. With Mul it spells a Q16.16 multiply:
               // the 64-bit product's middle word is (Mulhi << 16) | (Mul >>> 16).
    Shl,       // dst = a << imm — also how toFixed(v) is spelled (imm 16)
    Shr,       // dst = a >> imm, LOGICAL (zero-filling). The low word of a 64-bit product is
               // unsigned, so a fixed multiply needs this rather than Sar for its bottom half.
    Sar,       // dst = a >> imm, ARITHMETIC (sign-filling) — toInt(v) is this with imm 16.
               // Logical would turn every negative fixed value into a large positive int.
    Call,      // dst = (*callFn)(&frame[imm], b, arena) — call a host-registered function.
               // `imm` is the frame slot where the arguments start and `b` is how many there are:
               // the parser stages every argument into consecutive slots, so a call carries a
               // POSITION and a COUNT rather than the values, and arity is bounded by frame slots
               // instead of by operand fields. Backends materialise the address themselves.
    CallScript,// call the script's OWN function, the one numbered `imm` in declaration order.
               // Distinct from Call because the target is a position in THIS program rather than a
               // host address: the backend emits a relative call to a label, so there is nothing to
               // materialize and nothing to stage. A function NUMBER rather than a position: this
               // pass's own rewrite shifts every index past its first insertion, so an IR index
               // named an op that no longer started a function. It is also what makes recursion
               // work, since the
               // callee's own prologue allocates a fresh frame per activation.
               //
               // THE CALLER PASSES THE HOST ARGUMENTS ON. Every function's prologue parks
               // buf/nLights/cpl/t/ctrls out of the argument registers into its own frame, because
               // that is how the host enters the block, so a call that passed nothing left the
               // callee parking whatever those registers held and its first control read faulted on
               // a null arena. Each backend reloads them from the frame before the call; where they
               // go is the per-ISA part.
               //
               // Every caller vreg IS preserved across one: each backend's callLabel saves the
               // pool and restores it, the same guarantee a builtin call gives, because a value
               // computed before the call and read after it (`a() + b()`) is ordinary in an
               // expression. `b` says whether the caller wants the callee's value, and when it does
               // the result is delivered into `dst` after the pool is back. The allocator reads
               // that same flag to decide the op defines `dst` (MoonLiveSpill's writesDst).
    ConstPtr,  // dst = the pointer in `ptr`: a full-width address materialized into a register.
               // Distinct from Const because `imm` is int32_t and a pointer is 64 bits on the
               // desktop, so an address cannot ride an immediate. Every backend already builds one
               // for a host call's target (arm64 movz + 3x movk, the devices a byte at a time), so
               // this generalizes a proven sequence rather than adding a mechanism.
               //
               // What it carries is a string a script wrote. The source buffer is freed the moment
               // the compile returns, so the text is interned into the compiled program and this
               // op hands the emitted code a pointer that outlives it.
    Inline,    // a host-registered inline op (inlineOp tag); operands a/b/c/d (op-specific)
    LoadCtrl,  // dst = ((const uint8_t*)kArg4)[imm] — read a control value byte at offset imm
    LoadCtrl32,  // dst = *(int32_t*)((const uint8_t*)kArg4 + imm): read a member's whole 4-byte
                 // SLOT. Every scalar occupies one, whatever its type: byte and bool are masked
                 // on the way in, so the slot's upper bytes are already what the type promises,
                 // and fixed and int are the raw word. Signed because that is the only reading
                 // that serves all four: a byte or bool slot never has bit 31 set.
    StoreCtrl32, // *(int32_t*)((uint8_t*)kArg4 + imm) = a: write a member's whole slot. Takes the
                 // offset as an IMMEDIATE, unlike StoreCtrl which burns a movImm into
                 // a scratch register first — the load path's shape, and one instruction shorter.
    LoadIdx,   // dst = arena[base + a * width]: read an ARRAY element, index in vreg `a`.
    StoreIdx,  // arena[base + a * width] = b: write an ARRAY element, index in vreg `a`.
               // base, width and count are PACKED INTO `imm` (idxPack/idxBase/idxWidth/idxCount),
               // NOT carried in c/d. Those are VREG fields, and the spill pass renumbers every
               // vreg an op reports as a source, so a width parked there is rewritten into a
               // register number: the array then addresses the wrong byte with the wrong stride.
               // Call documents the same trap for its argument count. This one reached a device,
               // because arm64's register map made the renumbering a no-op and every host test
               // passed; only Xtensa showed it, as a fixture that stayed dark.
               //
               // The pack lets the lowering scale the index and bounds-check it: an out-of-range index is CLAMPED to the
               // last element rather than faulting, because a script computes indices from live
               // control values and a fixture must degrade visibly, never crash (the robustness
               // rule). Clamping keeps every write inside the member, so it cannot corrupt the
               // system variables or the depth counter that share the arena.
    StoreCtrl, // ((uint8_t*)kArg4)[imm] = a: write an arena byte, which is what a MEMBER is.
               // A control is read-only to the script (the UI owns its value); a member is the
               // script's own state, so it needs the other direction. Same storage and the same
               // offset space, so the only new thing is the store. An arena byte outlives every
               // call and keeps its address across a recompile, which is exactly what "survives
               // the tick" means, and is why a stateful effect needs this op and not a frame slot.
    Mov,       // dst = a — the assignment a loop variable needs (vregs are otherwise write-once)
    Label,     // a branch target; `imm` is the label id. Emits no instruction.
    BranchGe,  // if (a >= b) goto label `imm` — UNSIGNED. The loop's ENTRY guard: skip a loop
               // whose range is empty, which is also what makes `for (i = 0; i < 0; …)` correct.
               //
               // STAYS unsigned, and BranchGeS is a separate op rather than a replacement, because
               // three of its users need unsigned and would break: the array-index clamp
               // (moonlive_lower.h) reads a negative index as a huge value so ONE branch catches
               // both ends of the range, the element-store bounds check does the same, and the
               // recursion-depth guard counts a byte. A loop counter is a count, so parseFor uses
               // this one too. Only a script's own comparison is signed.
    BranchGeS, // if (a >= b) goto label `imm`, SIGNED: the comparison a script writes. Separate
               // from BranchGe per the note above; every backend switch is exhaustive over IrOp,
               // so a backend that forgets it fails to COMPILE rather than silently comparing the
               // wrong way, which is why a width lives in its own op rather than a field.
    BranchNe,  // if (a != b) goto label `imm` — the BACKWARD edge that closes the loop.
    Ret,       // return from the enclosing function, with the value in `a` when `imm` is 1.
               //
               // Does NOT emit a bare return instruction: the epilogue also decrements the recursion
               // depth counter, so an early exit that jumped straight to `ret` would leak a level
               // per call. This lowers to a jump to the function's ONE exit, which the lowering
               // binds ahead of that decrement, so every path out of a function unwinds the same
               // way. A value is parked in the ABI's return register first (retValue), because the
               // teardown is what makes that register visible to the caller.
    Spill,     // frame slot `imm` = a   — a value the register file could not hold, parked
    Reload,    // dst = frame slot `imm` — the same value brought back for one use
};

// Spill/Reload are the register allocator's output, never the parser's: MoonLiveSpill rewrites a
// program that names more live values than the target has registers into one that fits, parking the
// overflow in the CALL FRAME (the textbook answer — a value that outlives the register file lives in
// memory). `imm` is a slot INDEX; each backend turns it into an offset from its own frame pointer,
// so the core pass never knows a stack layout and the backends never know the algorithm.

// Why these two branches and no unconditional jump: a bottom-tested loop needs exactly an entry
// guard and a back edge, and every backend here already has both (bgeu / bne on Xtensa and RISC-V,
// cmp + b.hs / b.ne on arm64). It is the shape FillElems has always lowered by hand, so the
// instruction sequence is proven on all three ISAs before a script could ever emit one.

struct IrInst {
    IrOp     op;
    VReg     dst = 0;
    VReg     a = 0, b = 0, c = 0, d = 0;   // source vregs (op-dependent)
    int32_t  imm = 0;                      // immediate (Const) / addr offset
    HostCallFn callFn = nullptr;           // Call: the host C function pointer (typed alias)
    const void* ptr = nullptr;             // ConstPtr: the address to materialize
    InlineOp inlineOp{};                   // Inline: the neutral opcode tag
};

// A control a script declared (`addControl("speed", speed, 0, 99)`). Neutral: the core
// knows {name, a neutral type, range, default, and the byte offset into the run-time controls
// arena it lives at}. The light-domain binding turns this into a real MoonModule control bound to
// the arena slot. `type` is a neutral kind — Uint8 only in Stage 1 — NOT a projectMM ControlType.
/// A member's element type. The WIDTH is derived from it rather than stored beside it, so the two
/// can never disagree: a record carrying both would let a `Uint16` claim one byte.
// CtrlType / ctrlWidth live in MoonLiveBuiltins.h: a builtin descriptor names the member
// width its by-reference argument takes, and that header cannot include this one.

struct DeclaredControl {
    const char* name = nullptr;        // script-declared name (points into the source buffer)
    // The UI range, as wide as the widest member a control can bind: a byte declares 0..255 and
    // an int member the full 32-bit span, so the field has to hold the wider one.
    int32_t     min = 0, max = 255;
    // The initializer, wide enough for the widest member type. Separate from the range because a
    // member may be seeded to a value outside what its slider spans. Signed and 32-bit because a
    // scalar occupies a 4-byte slot: an `int` member's range and default span the whole type.
    int32_t     def = 0;
    uint8_t     nameLen = 0;           // length (the source is not NUL-terminated per token)
    CtrlType    type = CtrlType::Int;
    // Byte offset into the controls arena, assigned as a running CURSOR in declaration order. Not
    // the declaration index: a scalar costs a whole 4-byte slot and an array costs count * element
    // width, so the n-th member is no longer at byte n. Everything downstream (the bindings'
    // cached slot pointers, persistence, addControl's by-reference argument) keys on this offset,
    // is why widening a member does not reach any of them.
    uint8_t     offset = 0;
    // Elements: 1 for a scalar, the length for an array. Total bytes is count * ctrlWidth(type).
    uint8_t     count = 1;
};

/// LoadIdx/StoreIdx pack (base, width, count) into the single `imm` field, because `imm` is the
/// only per-instruction field the register allocator does not rewrite. One encoder and three
/// accessors, so the emitter and the lowering cannot disagree about the layout.
constexpr int32_t idxPack(uint8_t base, uint8_t width, uint8_t count) {
    return int32_t(base) | (int32_t(width) << 8) | (int32_t(count) << 16);
}
constexpr uint8_t idxBase(int32_t p)  { return uint8_t(p & 0xff); }
constexpr uint8_t idxWidth(int32_t p) { return uint8_t((p >> 8) & 0xff); }
constexpr uint8_t idxCount(int32_t p) { return uint8_t((p >> 16) & 0xff); }


/// Branch targets one IR program may use. Two per `for` (entry guard + back edge), and the counter
/// runs for the whole program rather than per scope — a label is never reused once a loop closes —
/// so this bounds the TOTAL number of loops in a script (8), not how deeply they nest. Nesting
/// depth is bounded separately, by `locals`.
/// Raised from 16 (2026-09-07). Sixteen bounded a script to eight loops, and a script that SELECTS
/// between modes spends them fast: each `else if` arm is two, so a five-mode effect with a loop in
/// each was refused with "too many branches" for asking something ordinary. The cost is four small
/// stack arrays in the spill pass and the lowering, a few hundred bytes on a machine that already
/// allocates its exec block from the heap. The ASSEMBLER tables did not move with it: they are
/// sized by named functions and StoreElems, which an `else if` arm does not add, and raising them
/// in step would have put lowerWith's frame past the 2 KB this file warns about below.
static constexpr uint8_t kIrLabels = 40;

/// Labels and fixups an ASSEMBLER's tables hold. Larger than kIrLabels because a backend allocates
/// labels the IR never names: one per named function (a call may precede its definition, so these
/// cannot be lazy), plus one per StoreElem for its bounds guard and two per FillElems. A class of
/// three functions each holding a loop and a store needs more than the sixteen that sized these
/// when a script was a single routine: crosshair.mle is exactly that script, and it failed to
/// compile with the generic "too large" rather than naming the table it exhausted.
///
/// Held in core so the three backends cannot drift: they carried an identical private copy each,
/// and a script that fit one would have been refused by another.
///
/// THE COST IS STACK, and it is the number to watch when raising these. Both tables are members of
/// the assembler, which is a local in lowerWith: 4 bytes per label, 8 per fixup on a 32-bit
/// target. Measured on the classic ESP32 image, `lowerWith` went from 480 to 1120 bytes, so 48/96
/// costs 640 bytes more than 16/32. That frame is the largest on the compile chain
/// (compileScriptFile 144 + MoonLive::compile 288 + compileSource 576 + lowerWith 1120 = 2128
/// nested), against CONFIG_ESP_MAIN_TASK_STACK_SIZE = 12288: 17% of the task at the deepest point.
/// Flash is unchanged, since these are stack arrays rather than data.
///
/// There is headroom, but not unlimited headroom, and a compile runs on the RENDER task. Doubling
/// these again would put lowerWith past 2 KB. If a future script needs more, move the tables to the
/// heap alongside the code buffer (which was moved for exactly this reason) rather than raising the
/// constants: this project has already bootlooped a P4 on an oversized stack frame.
static constexpr uint8_t kAsmLabels = 48;
static constexpr uint8_t kAsmFixups = 96;

/// Script variables live in FRAME SLOTS, and this bounds how many one program may hold at once:
/// named variables, loop counters and limits, and the arguments staged for a call, all at the same
/// moment. A block hands its slots back at its closing brace and a call hands back its staging, so
/// the budget is what is LIVE at once rather than a total.
///
/// 32 is what a volumetric script needs: three nested loops cost six slots before a five-argument
/// write stages anything. The budget's cost is STACK, not encoding (Xtensa's s32i/l32i offset
/// reaches 256 slots, RISC-V's 2048, and the host addresses from a frame pointer), and it is
/// measured: the script's own frame is 148 bytes on the render task, and only for a script that
/// uses the slots, since the prologue reserves the peak the program actually reached; the compile
/// chain's deepest nesting is 2512 bytes of the main task's 12288, at 24 bytes per parser local.
static constexpr uint8_t kMaxLocals = 32;

/// The most arguments one call can carry. Bounded by the FRAME, not by the register file: the parser
/// stages arguments into consecutive local slots, so a call can take as many as the locals range
/// holds — which is what makes a seven-argument `draw::line` expressible at all. `push` validates a
/// Call's count against this instead of against kMaxVRegs, which would cap arity at the register
/// count the frame staging exists to escape.
static constexpr uint8_t kMaxCallArgs = kMaxLocals;

/// Where the HOST ARGUMENTS are parked. buf/nLights/cpl/t/ctrls arrive in registers and are never
/// written, but holding them there costs FIVE registers for the whole program — on Xtensa, five of
/// the ten the windowed ABI leaves, which is the difference between a script compiling and not. They
/// are stored to these slots once at entry and reloaded where read, exactly like a script variable.
///
/// The slots sit at the TOP of the addressable range, above everything the parser and the register
/// allocator hand out (both number upward from zero), so neither can collide with them.
static constexpr uint8_t kHostArgSlots = kFirstTemp;                 // 5
constexpr uint8_t hostArgSlot(VReg v) {
    return static_cast<uint8_t>(kMaxLocals + kHostArgSlots - kFirstTemp + v);
}
/// Total frame slots a backend must be able to address: the parser/allocator range plus the parked
/// host arguments above it.
static constexpr uint8_t kTotalSlots = kMaxLocals + kHostArgSlots;

/// Named functions one program may define. Mirrors kMaxEntryPoints in the compiler header:
/// the IR carries the offsets, the CompileResult carries the names, and the two are filled
/// from the same parse, so they are bounded together.
static constexpr uint8_t kMaxIrEntries = 8;


static constexpr uint8_t kMaxControlName = 24;   // max control-name length (incl. NUL); the compiler
                                                 // rejects longer names so the binding's name pool
                                                 // can't truncate distinct names into a collision

// A lowered program: the ops, sized to the script, plus the vreg high-water mark.
//
// The op array is HEAP-ALLOCATED rather than an `IrInst ops[kMaxIrOps]` member. As a member it cost
// the same ~2 KB of STACK for a one-statement script as for a full one, and this object is a local
// on the compile path of a 12 KB main task — so raising the ceiling by growing the array would have
// traded a compile limit for a stack overflow (this project has already lost a P4 to a large stack
// frame). Sizing to the script makes the small case cheaper AND the large case possible.
// Compilation is cold path, so the allocation costs nothing that matters.
//
// Ownership is RAII: one allocation, freed in the destructor, copying deleted. There is no manual
// free path to miss — the reverted 32026eb5 turned four tables into independently-nullable pointers
// and its own comment records the heap corruption that followed from missing one guard.
struct IrProgram {
    IrInst*  ops = nullptr;
    uint16_t cap = 0;                      // entries allocated
    uint16_t count = 0;
    VReg     vregsUsed = kFirstTemp;
    /// Frame slots the FRONT END allocated for script variables. Slot indices 0..localSlots-1 are
    /// already spoken for when the backend sizes its prologue, and the register allocator numbers
    /// any spill it still needs from here up — the two share one frame, so they cannot both start
    /// at zero without a variable and a spilled temp landing on the same bytes.
    uint8_t  localSlots = 0;

    /// Where each named function's code STARTS, filled in as the lowering walks the ops.
    ///
    /// The front end knows which IR index a function begins at, but not which byte: that is decided
    /// by the encoding, which is the backend's business. So the parser records the index in
    /// `fnIrStart` and the lowering fills `fnOffset` as it passes it. This is the same crossing a
    /// linker makes between a symbol and its address, done in one pass because there is one section.
    ///
    /// `fnCount` is 0 for a program with no named functions (the hand-encoded fills), and the
    /// binding then calls the block start, which is what it has always done.
    uint16_t fnIrStart[kMaxIrEntries] = {};
    uint16_t fnOffset[kMaxIrEntries]  = {};
    uint8_t  fnCount = 0;

    IrProgram() = default;
    ~IrProgram() { platform::free(ops); }
    IrProgram(const IrProgram&) = delete;              // owns a buffer; a copy would double-free
    IrProgram& operator=(const IrProgram&) = delete;

    /// Size the op array to `n` entries. False when the allocation fails or `n` exceeds the sanity
    /// bound, so the caller reports a diagnostic instead of writing through a null pointer.
    bool reserve(uint16_t n) {
        if (n == 0 || n > kMaxIrOps) return false;
        platform::free(ops);
        ops = static_cast<IrInst*>(platform::alloc(sizeof(IrInst) * n));
        cap = ops ? n : 0;
        count = 0;
        return ops != nullptr;
    }

    bool push(const IrInst& i) {
        if (!ops || count >= cap) return false;
        // Reject any op that names a vreg outside the fixed register budget — an invalid program
        // is dropped at the seam rather than reaching a backend that would index past its map.
        //
        // OPCODE-SPECIFIC, because not every operand field holds a vreg. For a Call, `b` is the
        // ARGUMENT COUNT and `imm` the frame-slot base — its arguments live in the frame, not in
        // registers. Validating `b` as a vreg capped calls at kMaxVRegs arguments, which is the very
        // limit staging through the frame exists to remove.
        if (i.dst >= kMaxVRegs) return false;
        if (i.op == IrOp::Call) {
            if (i.b > kMaxCallArgs) return false;   // a count, not a register
        } else if (i.a >= kMaxVRegs || i.b >= kMaxVRegs ||
                   i.c >= kMaxVRegs || i.d >= kMaxVRegs) {
            return false;
        }
        ops[count++] = i;
        if (i.dst + 1 > vregsUsed) vregsUsed = static_cast<VReg>(i.dst + 1);
        return true;
    }

    /// Exchange contents with `o`. The register allocator builds the rewritten program in a second
    /// IrProgram (inserting a Reload before a use and a Spill after a def cannot be done in place in
    /// a right-sized array) and swaps it in; the old buffer then dies with the local. Copy is deleted
    /// precisely because two owners would double-free, so a swap is how ownership moves here.
    void swap(IrProgram& o) {
        IrInst* p = ops; ops = o.ops; o.ops = p;
        uint16_t t = cap; cap = o.cap; o.cap = t;
        t = count; count = o.count; o.count = t;
        VReg v = vregsUsed; vregsUsed = o.vregsUsed; o.vregsUsed = v;
        uint8_t ls = localSlots; localSlots = o.localSlots; o.localSlots = ls;
        // The FUNCTION TABLE is part of the program, so it moves with the ops. Omitting it left the
        // spill pass's remapped boundaries in the discarded half while the lowering read the stale
        // pre-spill ones: it then opened a function's frame two ops early, in the middle of the
        // previous function's pixel write, and the emitted block was structurally plausible enough
        // (one entry and one retw per function) that only a disassembly showed it.
        uint8_t fc = fnCount; fnCount = o.fnCount; o.fnCount = fc;
        for (uint8_t f = 0; f < kMaxIrEntries; f++) {
            uint16_t s = fnIrStart[f]; fnIrStart[f] = o.fnIrStart[f]; o.fnIrStart[f] = s;
            uint16_t b = fnOffset[f];  fnOffset[f]  = o.fnOffset[f];  o.fnOffset[f]  = b;
        }
    }

    /// Which inline ops this program contains, so a backend reserves scratch only for what is there.
    ///
    /// The backends reserved their maximum unconditionally, which cost a register no matter what the
    /// script did — and that one register is what made a nested loop refuse to compile on the
    /// smallest target, since a layout script neither fills nor stores elements. How MANY scratch
    /// registers each op costs is per-ISA (the host needs one for StoreElem, which Xtensa and RISC-V
    /// fold into the index vreg) and stays with each backend; WHICH ops are present is a property of
    /// the program, so it is answered once here.
    bool hasInline(InlineOp which) const {
        for (uint16_t i = 0; i < count; i++)   // uint16_t: `count` is, so a uint8_t never terminates
            if (ops[i].op == IrOp::Inline && ops[i].inlineOp == which) return true;
        return false;
    }

    /// Does any function call another function of this script? The recursion depth guard is emitted
    /// only when one does, so a script that just defines `tick()` (every shipped script today)
    /// carries none of it.
    bool hasScriptCall() const {
        for (uint16_t i = 0; i < count; i++)
            if (ops[i].op == IrOp::CallScript) return true;
        return false;
    }
};

}  // namespace mm::moonlive
