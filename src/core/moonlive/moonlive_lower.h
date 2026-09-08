#pragma once

#include "core/moonlive/moonlive_emit.h"
#include "core/moonlive/MoonLiveIr.h"
#include "core/moonlive/MoonLiveSpill.h"   // the register allocator, run before lowering

#include <cstring>

// MoonLive IR -> machine bytes: the ONE lowering, written once for every backend.
//
// Walking the IR is not a per-target algorithm. Which register holds a value, which frame slot it
// spills to, when a host argument is reloaded, how an inline op is expanded: all of that is decided
// by core, identically for arm64, RISC-V and Xtensa. What differs per target is how an instruction
// is ENCODED, and that already lives behind the assembler. So the walk lives here and each backend
// supplies its assembler.
//
// This was three near-identical files. The two device lowerings differed by two identifier tokens,
// and the host one by a handful of lines that turned out to be free choices rather than ISA facts.
// The risk that shape carries is not the duplication itself but the silence: a rule changed in one
// copy and not the others miscompiles on one target while the tests, which execute only the host
// backend, stay green.
//
// The assembler contract, which all three satisfy:
//   ctor(size_t cap), newLabel, bind, prologue(uint8_t), epilogue, alignForEntry, finalize,
//   bytes, size, overflowed, spillStore, spillLoad, slotAddr,
//   movImm, movPtr, movReg, addImm, addReg, mulReg, mulhi, shlImm, shrImm, sarImm,
//   store8, load8, load32, store32, load8Idx, load32Idx, store32Idx,
//   branchIfZero, branchGeU, branchNe, call, callLabel, and kMaxSpillSlots.
// The branches are the FUSED forms (compare-and-branch as one call). arm64 has no such
// instruction and spells each as cmp + b.cond inside its assembler, which is exactly where a
// difference of encoding belongs.
//
// INCLUDE ORDER: a backend includes its own assembler header BEFORE this one. `Reg` and `Label`
// are declared per assembler (each ISA's register file is a different size, which is the whole
// point of kRegCount), so this header names them without declaring them: it is the algorithm, not
// the register set. That is also why it is a template rather than a compiled unit; there is no
// single `Reg` for it to compile against.

namespace mm::moonlive {

/// Lower `ir` into `out` using assembler `A`. Returns the byte count, or 0 when the program cannot
/// be encoded (degrade, never miscompile).
template <typename A>
size_t lowerWith(IrProgram& ir, uint8_t* out, size_t cap, const RegBudget* squeeze,
                 uint8_t regCount) {
    // Taken FROM the assembler, not named globally: each backend declares its own `Reg` (the
    // register files differ in size, which is what kRegCount is) and its own `Label`. Deducing
    // them from the assembler keeps this header free of any one ISA's types, so it compiles as
    // core code rather than needing a backend included first. RegId/LabelId rather than Reg/Label:
    // the backend's own names are already in scope wherever this is instantiated, and reusing them
    // shadows those declarations (-Wshadow is an error here).
    using RegId = typename A::RegType;
    // LabelId, not Label: every assembler declares `using Label = uint8_t` in its own namespace,
    // so reusing that name here would shadow it. Deduced from the ASSEMBLER INSTANCE below rather
    // than through std::declval, because the codegen tests compile a backend inside a wrapper
    // namespace, and a <utility> include here would land in it and nest a second `std`.
    auto reg = [](VReg v) { return static_cast<RegId>(v); };

    // THREE scratch registers, for every program. Two are shared by everything that needs a
    // temporary for the length of one instruction, and the third is the host-argument reload.
    //
    // The two shared ones (sAddr, sCtr below) serve, in turn: FillElems (loop counter +
    // per-channel address), StoreElem (the address, which must NOT be folded into the caller's
    // index vreg, since that destroys a `for` counter), a host Call (its argument block address
    // and count), and the recursion depth guard (the counter and its comparand). None of those
    // is live at the same time as another, because each dies inside the one instruction that
    // uses it, so they share rather than reserve separately: reserving per user cost registers
    // permanently, which on Xtensa's ten is the difference between compiling and not.
    //
    // NOT conditional on which inline ops the program contains, and the floor of two is what
    // makes that safe. It reads like a missed optimization, and was one until the depth guard
    // arrived: the guard is emitted in a function's prologue and uses both shared registers, in
    // a script that may contain no inline op at all. A count derived from `hasInline` alone
    // would hand the guard a register the program does not own.
    //
    // The +1 is the host-argument reload: the host arguments live in frame slots (core parks
    // them at entry), so an op that reads buf/nLights/cpl/ctrls brings one back for its own
    // instruction.
    constexpr uint8_t kSharedScratch = 2;
    const uint8_t scratchTotal = kSharedScratch + 1;
    if (!out || cap == 0) return 0;

    // Run the register allocator before lowering. It leaves a program that already fits untouched,
    // and rewrites one that does not into Spill/Reload against this backend's frame, replacing the
    // hand-rolled bail that used to REFUSE such a script outright. False here means even the
    // spilled form does not fit, which is a diagnostic, never a miscompile.
    uint8_t slots = 0;
    // `squeeze` overrides the REGISTER COUNT and slot count a test wants to constrain, but never
    // `reserved`: the scratch is what this lowering is about to use for its inline ops and call
    // argument block, so a test-supplied value would let the allocator hand out a register the
    // lowering then overwrites, miscompiling exactly the squeezed programs the seam exists to prove.
    const RegBudget budget = squeeze ? RegBudget{squeeze->regs, scratchTotal, squeeze->slots}
                                     : RegBudget{regCount, scratchTotal, A::kMaxSpillSlots};
    if (!spillToBudget(ir, budget, slots)) return 0;
    // sAddr FIRST: it is the one StoreElem also uses, and a store-only program reserves a single
    // scratch, so the shared one has to be the lowest index or it would name an unreserved register.
    const RegId sAddr = static_cast<RegId>(ir.vregsUsed);       // per-channel address (both ops)
    const RegId sCtr  = static_cast<RegId>(ir.vregsUsed + 1);   // FillElems loop counter

    // Size the assembler's buffer to the CALLER's: `cap` is what the staging buffer holds, so the
    // two can never disagree about how much a script may emit (they were separately constant, and
    // a script that fit one overflowed the other).
    A a(cap);
    using LabelId = decltype(a.newLabel());
    // The LAST reserved scratch index, derived from scratchTotal rather than hard-coded: the `+1`
    // in scratchTotal above IS this register, so the reservation and the use cannot drift apart.
    // A fixed offset sat OUTSIDE the reservation and only worked because the register maps happen
    // to have spare entries above the high-water mark.
    const RegId sHost = static_cast<RegId>(ir.vregsUsed + scratchTotal - 1);
    auto host = [&](VReg v) -> RegId { a.spillLoad(sHost, hostArgSlot(v)); return sHost; };
    // The frame must cover the parked HOST ARGUMENTS at the top as well as whatever the parser and
    // the allocator claimed at the bottom: they are stored before any script code runs, so a frame
    // sized only from `slots` would put them past its end.
    const uint8_t frameSlots = slots > kTotalSlots ? slots : kTotalSlots;
    // A program with NO named functions is one routine, so its prologue opens the block. A class
    // gets a prologue PER FUNCTION instead, emitted at each function's first op below, because an
    // entry point's recorded offset has to be an address a caller can jump to: pointing it past a
    // single program-wide prologue would enter a routine whose frame was never established.
    if (ir.fnCount == 0) a.prologue(frameSlots);

    // An IR label id becomes an assembler label ON FIRST USE. Allocating the whole range up front
    // exhausts the assembler's fixed label table, and the inline ops (StoreElem's bounds guard,
    // FillElems' loop) then get nothing when they ask for their own, which broke every program that
    // contains no loop at all. Lazy allocation costs one lookup and leaves the table for the labels
    // a program actually has.
    // One label per named function, allocated up front: a CallScript may appear BEFORE the
    // function it targets (and does, for recursion), so the label has to exist before it is bound.
    LabelId fnLabel[kMaxIrEntries];
    for (uint8_t f = 0; f < ir.fnCount; f++) fnLabel[f] = a.newLabel();

    // The depth guard is emitted only for a script whose functions call each other: it is dead
    // weight in every script that just defines `tick()`, which is all of the shipped ones.
    const bool guardDepth = ir.hasScriptCall();
    // Where a function jumps when it is too deep: its own epilogue, so the refusal unwinds through
    // the same decrement and return as a normal exit and there is no second way out of a frame.
    LabelId tooDeep[kMaxIrEntries];
    if (guardDepth)
        for (uint8_t f = 0; f < ir.fnCount; f++) tooDeep[f] = a.newLabel();
    // Where an early `return` jumps: the function's ONE exit, bound by closeFn ahead of the depth
    // decrement so a return unwinds exactly as running off the end does. Allocated for every
    // function whether or not it returns early, because the label is what closeFn binds; unused
    // ones bind and emit nothing. Not shared with tooDeep: that one only exists when the script
    // calls, and a return needs an exit either way.
    // Zero-initialized, though only the first `fnCount` entries are ever read: GCC 14 cannot see
    // that bound across the lambdas below and reports a maybe-uninitialized use under -Werror,
    // where GCC 16 and clang stay quiet. A compiler-version difference is not worth an exception,
    // and the initializer costs nothing on a cold path.
    LabelId fnExit[kMaxIrEntries] = {};
    for (uint8_t f = 0; f < ir.fnCount; f++) fnExit[f] = a.newLabel();
    // Which function is being emitted, so a Ret knows which exit is its own. -1 until the first
    // function opens: a function-less program (the hand-built IR the codegen tests use) has no
    // exit label to jump to, and its Ret is refused rather than jumping to label -1.
    int curFn = -1;
    // Function number + 1 while a guard is owed, 0 when none is: the guard is emitted a few ops
    // after the prologue, once the host arguments have been parked.
    int guardPending = 0;

    LabelId labels[kIrLabels];
    bool  labelMade[kIrLabels] = {};
    auto  labelFor = [&](int32_t id) -> LabelId {
        if (!labelMade[id]) { labels[id] = a.newLabel(); labelMade[id] = true; }
        return labels[id];
    };

    // Emit this function's depth increment, if one is still owed.
    //
    // It goes AFTER the function's host-argument Spills, not before them. Both the guard and the
    // epilogue address the arena through the PARKED copy in the frame, and a refusing activation
    // jumps straight to the epilogue, so a guard placed first would make the refusal read a frame
    // slot nothing had written yet. Emitting it once the parking is done means the two agree by
    // construction rather than by argument.
    //
    // Called both at the first non-Spill op and from closeFn, because a function may have NO other
    // op: `nop() {}` parses, and its whole body is the five parking Spills. Flushed only at the
    // first site, that function got no increment while its epilogue still decremented, so every
    // call to it drove the counter DOWN. Two such calls wrapped the byte to 255 and the next real
    // call was refused as too deep, which reads as a legal call silently doing nothing.
    auto flushGuard = [&]() {
        if (!guardPending) return;
        const uint8_t f = static_cast<uint8_t>(guardPending - 1);
        guardPending = 0;
        const RegId d = host(kArg4);              // one reload; nothing below clobbers it
        a.load8(sAddr, d, kDepthSlot);
        a.movImm(sCtr, 1);
        a.addReg(sAddr, sAddr, sCtr);             // depth + 1
        a.movImm(sCtr, kDepthSlot);
        a.store8(d, sCtr, sAddr);                 // arena[kDepthSlot] = depth + 1
        a.movImm(sCtr, kMaxCallDepth);
        a.branchGeU(sAddr, sCtr, tooDeep[f]);     // at or past the limit: unwind immediately
    };

    // Close function `f`: the one exit every activation takes, whether it ran to the end or refused
    // at the guard. Written once so the two cannot drift: a refusal that skipped the decrement
    // would leak a level per attempt and shrink the budget until nothing recursed at all.
    auto closeFn = [&](uint8_t f) {
        flushGuard();          // an empty function still balances: increment, then decrement
        // Every early `return` lands here, AHEAD of the decrement below: an exit that skipped it
        // would leak a depth level per call, which is the same bug the too-deep path documents.
        a.bind(fnExit[f]);
        if (guardDepth) {
            a.bind(tooDeep[f]);                      // the too-deep path joins here
            const RegId d = host(kArg4);
            a.load8(sAddr, d, kDepthSlot);
            a.movImm(sCtr, -1);
            a.addReg(sAddr, sAddr, sCtr);            // depth - 1
            a.movImm(sCtr, kDepthSlot);
            a.store8(d, sCtr, sAddr);
        }
        a.epilogue();
    };

    // uint16_t, matching IrProgram::count: the op array is sized to the script now, so a uint8_t
    // counter wrapped at 256 ops and looped forever instead of emitting.
    for (uint16_t i = 0; i < ir.count; i++) {
        // A named function starts here. Close the previous one and open this one, so every function
        // is independently callable: its recorded offset is its PROLOGUE, and it returns rather than
        // running on into whatever was emitted next.
        for (uint8_t f = 0; f < ir.fnCount; f++) {
            if (ir.fnIrStart[f] != i) continue;
            if (f > 0) closeFn(static_cast<uint8_t>(f - 1));   // the previous function returns
            curFn = f;
            // Align BEFORE recording the offset, so the recorded address is the one a caller
            // actually jumps to. On Xtensa a function entry must be 4-byte aligned or the call
            // cannot be encoded and the instruction itself is illegal; the other backends are
            // fixed-width and this costs them nothing.
            a.alignForEntry();
            // Recorded AFTER the epilogue and BEFORE the prologue: the offset is the first byte a
            // caller executes, which is the frame setup, not the first statement.
            ir.fnOffset[f] = static_cast<uint16_t>(a.size());
            a.bind(fnLabel[f]);                      // where a CallScript to this function lands
            a.prologue(frameSlots);
            // THE RECURSION DEPTH GUARD, in the CALLEE's prologue: the textbook placement, one copy
            // per function rather than one per call site, and none at all for a script that never
            // calls (which is every shipped script today).
            //
            //     if (++arena[kDepthSlot] >= kMaxCallDepth) return;
            //
            // A refusing callee RETURNS rather than the caller skipping the call. That is what
            // makes the guard cheap: no branch-around at every call site, no counter to restore on
            // the way back, and one exit path. The counter is decremented in the epilogue, so it
            // unwinds with the frames that incremented it.
            //
            // Degradation is visible and the device keeps rendering: the deepest calls do nothing,
            // so the picture is wrong where the recursion bottomed out, rather than the whole
            // device resetting. That is what the robustness rule asks for.
            guardPending = guardDepth ? int(f) + 1 : 0;   // emit it after the args are parked
        }

        if (ir.ops[i].op != IrOp::Spill) flushGuard();

        const IrInst& op = ir.ops[i];
        switch (op.op) {
            case IrOp::Const:  a.movImm(reg(op.dst), op.imm); break;
            case IrOp::ConstPtr: a.movPtr(reg(op.dst), op.ptr); break;
            case IrOp::Add:    a.addReg(reg(op.dst), reg(op.a), reg(op.b)); break;
            case IrOp::AddImm: a.addImm(reg(op.dst), reg(op.a), op.imm); break;
            case IrOp::Mul:    a.mulReg(reg(op.dst), reg(op.a), reg(op.b)); break;
            case IrOp::Mulhi:  a.mulhi(reg(op.dst), reg(op.a), reg(op.b)); break;
            // The shift amount is an immediate 1..31. A zero shift is a no-op the front end never
            // emits (Xtensa cannot even encode it: slli's field holds 32-n).
            case IrOp::Shl:
                // A shift of 0 is a move; anything outside 1..31 has no encoding and must REFUSE
                // rather than silently become one, which is the stance Xtensa's shrImm takes for
                // the same reason: a wrong constant that still runs is the worst outcome.
                if (op.imm > 0 && op.imm < 32) a.shlImm(reg(op.dst), reg(op.a), uint8_t(op.imm));
                else if (op.imm == 0) { if (op.dst != op.a) a.movReg(reg(op.dst), reg(op.a)); }
                else a.shlImm(reg(op.dst), reg(op.a), 32);   // no encoding: the assembler refuses
                break;
            case IrOp::Shr:
                // A shift of 0 is a move; anything outside 1..31 has no encoding and must REFUSE
                // rather than silently become one, which is the stance Xtensa's shrImm takes for
                // the same reason: a wrong constant that still runs is the worst outcome.
                if (op.imm > 0 && op.imm < 32) a.shrImm(reg(op.dst), reg(op.a), uint8_t(op.imm));
                else if (op.imm == 0) { if (op.dst != op.a) a.movReg(reg(op.dst), reg(op.a)); }
                else a.shrImm(reg(op.dst), reg(op.a), 32);   // no encoding: the assembler refuses
                break;
            case IrOp::Sar:
                // A shift of 0 is a move; anything outside 1..31 has no encoding and must REFUSE
                // rather than silently become one, which is the stance Xtensa's shrImm takes for
                // the same reason: a wrong constant that still runs is the worst outcome.
                if (op.imm > 0 && op.imm < 32) a.sarImm(reg(op.dst), reg(op.a), uint8_t(op.imm));
                else if (op.imm == 0) { if (op.dst != op.a) a.movReg(reg(op.dst), reg(op.a)); }
                else a.sarImm(reg(op.dst), reg(op.a), 32);   // no encoding: the assembler refuses
                break;
            // A real register move, NOT add-immediate-zero: Xtensa's addi.n cannot encode 0, since
            // the ISA reuses that slot for -1, so `dst = a + 0` silently computed a - 1. A loop
            // counter initialized through Mov therefore started at -1, the unsigned loop guard saw
            // 0xffffffff >= limit, and the body never ran. It compiled, reported no error, and
            // placed no lights.
            case IrOp::Mov:    a.movReg(reg(op.dst), reg(op.a)); break;
            case IrOp::Label:
                if (op.imm >= 0 && op.imm < kIrLabels) a.bind(labelFor(op.imm));
                break;
            case IrOp::BranchGe:
                if (op.imm >= 0 && op.imm < kIrLabels)
                    a.branchGeU(reg(op.a), reg(op.b), labelFor(op.imm));
                break;
            case IrOp::BranchGeS:
                if (op.imm >= 0 && op.imm < kIrLabels)
                    a.branchGeS(reg(op.a), reg(op.b), labelFor(op.imm));
                break;
            case IrOp::BranchNe:
                if (op.imm >= 0 && op.imm < kIrLabels)
                    a.branchNe(reg(op.a), reg(op.b), labelFor(op.imm));
                break;
            case IrOp::LoadCtrl: a.load8(reg(op.dst), host(kArg4), op.imm); break;   // dst = ctrls[imm]
            // A member's whole 4-byte SLOT. Unlike the byte and halfword stores below, the offset
            // rides the instruction as an immediate: nothing computes a slot address at run time,
            // so there is no reason to spend a register and a movImm on a constant.
            case IrOp::LoadCtrl32: a.load32(reg(op.dst), host(kArg4), op.imm); break;
            case IrOp::StoreCtrl32: a.store32(host(kArg4), op.imm, reg(op.a)); break;
            // An ARRAY element. `imm` is the array's base, op.c the element width and op.d the
            // element count, so both the scaling and the bound come from the IR rather than from a
            // rule the backends would each have to know.
            //
            // The index is CLAMPED, not checked-and-skipped. A script computes an index from live
            // control values, so out of range is an ordinary run-time state rather than a defect,
            // and every arena write has to stay inside the member: the system variables and the
            // recursion depth counter share this allocation, and a stray write would corrupt the
            // engine rather than the picture. Clamping costs one compare and one conditional
            // branch, which is what a bounds check costs anyway.
            case IrOp::LoadIdx:
            case IrOp::StoreIdx: {
                const uint8_t width = idxWidth(op.imm);
                const uint8_t count = idxCount(op.imm);
                const RegId idx = reg(op.a);
                // clamp: if (idx >= count) idx = count - 1, as ONE branch. sCtr holds the LAST
                // valid index rather than the count, so the guard is `last >= idx`, which is true
                // exactly when idx is in range and skips the fix-up. branchGeU is unsigned and the
                // index is unsigned, so this also catches a negative index: it arrives as a huge
                // unsigned value, fails the test, and clamps to the last element like any other
                // out-of-range one.
                const LabelId inRange = a.newLabel();
                a.movImm(sCtr, count - 1);
                a.branchGeU(sCtr, idx, inRange);
                a.movReg(idx, sCtr);
                a.bind(inRange);
                // idx *= width, as a SHIFT rather than a multiply. ctrlWidth produces only 1 and
                // 4 (see below), so the scaling is a compile-time power of two and the general
                // form was paying for a generality that cannot arise: width 1 multiplied by one,
                // which is two instructions and a scratch register to compute the number it
                // already had, and width 4 ran a full integer multiply where a shift says the same
                // thing. This runs on EVERY array access, so an effect holding per-item state
                // (a particle array, a byte[] heat map) pays it per light.
                //
                // In the shared lowering rather than per backend: every assembler already has
                // shlImm, so all four get it without a new primitive to implement four times.
                if (width == 4)      a.shlImm(idx, idx, 2);
                else if (width != 1) { a.movImm(sAddr, width); a.mulReg(idx, idx, sAddr); }
                a.addImm(idx, idx, idxBase(op.imm));    // ... plus the array's base
                // Two element widths, which is all ctrlWidth can produce: 1 for byte[] and
                // bool[], 4 for int[] and fixed[].
                if (op.op == IrOp::LoadIdx) {
                    if (width == 4) a.load32Idx(reg(op.dst), host(kArg4), idx);
                    else            a.load8Idx(reg(op.dst), host(kArg4), idx);
                } else {
                    if (width == 4) a.store32Idx(host(kArg4), idx, reg(op.b));
                    else            a.store8(host(kArg4), idx, reg(op.b));
                }
                break;
            }
            case IrOp::StoreCtrl: {
                // ctrls[imm] = a. store8 addresses through a REGISTER holding the offset, because
                // it is the per-light writer's shape where the index is computed, so the constant
                // goes into the shared scratch first. sCtr rather than sAddr: store8 clobbers its
                // own address temp on some backends, and sAddr is that temp.
                const RegId arena = host(kArg4);
                a.movImm(sCtr, op.imm);
                a.store8(arena, sCtr, reg(op.a));
                break;
            }
            // The allocator's two ops. `imm` is a slot INDEX; the assembler owns the frame layout.
            case IrOp::CallScript: {
                // `imm` is the callee's function NUMBER, so the label is a direct index. The
                // callee's address is not known when the call is emitted (it may be defined further
                // down, and for recursion it is the enclosing function), so this binds a label and
                // the assembler patches the displacement once every function is placed.
                if (op.imm < 0 || op.imm >= ir.fnCount) break;
                // `b` marks a call whose value the caller wants; the backend delivers it into
                // dst, since which register holds it is ABI knowledge (Xtensa's call8 rotates the
                // window, so the callee's a2 is the caller's a10).
                a.callLabel(fnLabel[op.imm], reg(op.dst), op.b != 0);
                break;
            }
            case IrOp::Ret:
                // The value first, then the jump: retValue parks it in the ABI's return register,
                // and the exit's teardown is what publishes that register to the caller.
                if (op.imm) a.retValue(reg(op.a));
                // A jump to the function's one exit, spelled as the always-taken BranchGe idiom the
                // compiler already uses for an unconditional jump, so no backend needs a new
                // instruction: `x >= x` holds whatever x is.
                //
                // The comparison uses host(kArg0), NOT op.a: a BARE return carries no value, so its
                // `a` field is uninitialized and reg() would map a nonsense vreg. kArg0 is the buf
                // pointer, always live and always mapped, and comparing it with itself reads it
                // without disturbing it.
                if (curFn >= 0) {
                    const RegId z = host(kArg0);
                    a.branchGeU(z, z, fnExit[curFn]);
                }
                break;
            case IrOp::Spill:  a.spillStore(reg(op.a), static_cast<uint8_t>(op.imm)); break;
            case IrOp::Reload: a.spillLoad(reg(op.dst), static_cast<uint8_t>(op.imm)); break;
            case IrOp::Call: {
                // The IR carries the host's function pointer (valid in the single flashed image) and
                // the FRAME SLOT its arguments start at. Hand the host their address and their
                // count: nothing is held in a register across the call, and how many arguments a
                // builtin takes stops being a property of this instruction.
                if (!op.callFn) return 0;
                const RegId argPtr = static_cast<RegId>(ir.vregsUsed);
                a.slotAddr(argPtr, static_cast<uint8_t>(op.imm));
                const RegId argN = static_cast<RegId>(ir.vregsUsed + 1);
                a.movImm(argN, static_cast<int32_t>(op.b));
                a.call(reg(op.dst), argPtr, argN,
                       host(kArg4), reinterpret_cast<const void*>(op.callFn));
                break;
            }
            case IrOp::Inline:
                switch (op.inlineOp) {
                    case InlineOp::StoreElem: {
                        LabelId skip = a.newLabel();
                        // The address goes in SCRATCH, not the index vreg: folding it in destroyed
                        // a `for` counter, which the loop's step and test read again after the store.
                        a.branchGeU(reg(op.a), host(kArg1), skip);
                        a.mulReg(sAddr, reg(op.a), host(kArg2));        // addr = index * cpl
                        a.store8(host(kArg0), sAddr, reg(op.b));
                        a.addImm(sAddr, sAddr, 1); a.store8(host(kArg0), sAddr, reg(op.c));
                        a.addImm(sAddr, sAddr, 1); a.store8(host(kArg0), sAddr, reg(op.d));
                        a.bind(skip);
                        break;
                    }
                    // Element 0, with no index to compute and no bounds test to make: the only way
                    // to be out of range is for the buffer to be empty, which nLights answers.
                    // Distinct from StoreElem rather than StoreElem with a constant, because it is
                    // a different question: "the one slot I was given" is not "slot number zero".
                    case InlineOp::StoreFirst: {
                        LabelId skip = a.newLabel();
                        a.branchIfZero(host(kArg1), skip);
                        a.movImm(sAddr, 0);
                        a.store8(host(kArg0), sAddr, reg(op.a));
                        a.addImm(sAddr, sAddr, 1); a.store8(host(kArg0), sAddr, reg(op.b));
                        a.addImm(sAddr, sAddr, 1); a.store8(host(kArg0), sAddr, reg(op.c));
                        a.bind(skip);
                        break;
                    }
                    case InlineOp::FillElems: {
                        LabelId done = a.newLabel(), top = a.newLabel();
                        a.movImm(sCtr, 0);
                        a.branchIfZero(host(kArg1), done);
                        a.bind(top);
                        a.mulReg(sAddr, sCtr, host(kArg2));
                        a.store8(host(kArg0), sAddr, reg(op.a));
                        a.addImm(sAddr, sAddr, 1); a.store8(host(kArg0), sAddr, reg(op.b));
                        a.addImm(sAddr, sAddr, 1); a.store8(host(kArg0), sAddr, reg(op.c));
                        a.addImm(sCtr, sCtr, 1);
                        a.branchNe(sCtr, host(kArg1), top);
                        a.bind(done);
                        break;
                    }
                }
                break;
            default: break;
        }
    }
    // Close the LAST function (or the single routine of a function-less program, which has no
    // guard because it cannot call).
    if (ir.fnCount > 0) closeFn(static_cast<uint8_t>(ir.fnCount - 1));
    else a.epilogue();
    a.finalize();
    if (a.overflowed() || a.size() > cap) return 0;
    std::memcpy(out, a.bytes(), a.size());
    return a.size();
}

}  // namespace mm::moonlive
