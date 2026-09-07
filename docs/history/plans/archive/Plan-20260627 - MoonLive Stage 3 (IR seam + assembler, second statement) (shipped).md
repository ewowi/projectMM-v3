# Plan — MoonLive Stage 3: the IR seam + a tiny assembler (second statement)

> Approved plan record (CLAUDE.md *Plan before implementing*). The next rung of MoonLive after the shipped 1a→1b→2 + P4 spike: add the **second statement kind** to the language, which forces the **typed IR** and a **per-ISA assembler** to earn their place (the current AST→emitFill shortcut only works for one fixed routine). Builds on [livescripts-analysis-top-down.md](../../../backlog/livescripts-analysis-top-down.md) §3.2 (IR seam), §4 (bounds-check at the IR), §3.4 (host built-ins).

## Goal

Compile `setRGB(i, r, g, b);` — a single-pixel write at a computed index — to native code, with a **bounds-check** so an out-of-range index can't overrun the buffer. This is the smallest statement that is *not a fill*: it has a computed store address (`i*cpl`), a guard, and no loop. Doing it honestly requires three things the spike deliberately deferred:

1. A **typed IR** — AST lowered to a flat list of three-address ops over virtual registers — so codegen stops being "patch 3 bytes into a fixed template" and becomes "lower a sequence of ops."
2. A **tiny per-ISA assembler** — named-instruction emitters (`movi`, `strb`, `add`, `cmp`, `b.cond`, `ret`) with **label back-patching** — because a multi-op statement's bytes must be *composed* at compile time (branch offsets, register liveness across ops), which can't be done as hand-grouped byte fragments (that's the StoreProhibited crash class the spike hit).
3. The **bounds-check as an IR op** so every backend inherits it and it's switchable by deleting nodes (doc §4).

The verbatim hand-encoded `fill` blobs from the spike do not die — they become the **golden regression fixture**, but as a **behavioral** anchor, not a byte-identical one: the assembler-built `fill` and the hand blob are both run over the same buffer and must produce **identical output**. (Byte-identical would force the clean assembler to mimic the hand blob's arbitrary register choices and instruction selection — coupling good code to an artifact. Real compilers assert behavioral equivalence + a size bound against a reference, not byte-equality; that's the *Common patterns first* choice here.) The assembler keeps its own clean register convention; the hand blobs stay as documented references the behavioral test pins against.

## Decisions locked with the product owner

- **Minimal IR + tiny assembler** (not a second hand-template, not a full SSA IR). ~7 ops, a fixed virtual-register file with last-use freeing — no SSA, no general register allocator (both deferred to *concrete-first* until a script exhausts registers).
- **Second statement = `setRGB(i, r, g, b)`**, in two half-steps: **3a** literal index (`setRGB(5, 0,0,255)`) — single write + index math + first BOUNDS, no host call; **3b** `setRGB(random16(N), …)` — adds the CALL op + the first host built-in, giving the tutorial hello-world.
- **Desktop (arm64/x86-64) first**, then Xtensa, then RISC-V — the stage-0.5 logic applied to codegen: prove the IR→assembler path on the in-process backend (instant feedback, golden-fill regression), then bring up the device ISAs against the proven IR.

## The IR (minimal, ~7 ops, no SSA)

A flat list of three-address ops over virtual registers `v0..vN` plus the named host args (`buf`, `nLights`, `cpl`, `t`). Defined once in the neutral core (`src/core/moonlive/`):

| Op | Meaning |
|---|---|
| `Const vd, imm` | load an integer immediate |
| `Mul/Add/Shl vd, va, vb` | integer arithmetic (index scaling `i*cpl`, range reduction) |
| `Bounds vidx, vlimit` | the §4 guard — skip the store if `vidx >= vlimit` (its own op so bounds on/off = delete the node, and every backend inherits it) |
| `Store buf, vaddr, vr, vg, vb` | write RGB at a computed byte address |
| `Call vd, builtin, varg…` | call a host built-in (`random16`), result in `vd` — the single seam for all host functions |
| `Loop vcounter, vlimit { … }` | a counted loop body (what makes `fill` a loop over all lights) |

- `fill(r,g,b)` = `Const×3` + `Loop{ Store }`.
- `setRGB(<lit>, r,g,b)` = `Const idx` + `Const×3` + `Bounds` + (`Mul idx,cpl`) + `Store`.
- `setRGB(random16(N), …)` = `Const N` + `Call random16` + `Bounds` + `Const×3` + `Store`.

The set is **closed under the rest of the ladder**: 2D/3D addressing is more index arithmetic (already have Mul/Add); oscillators add float variants; Ripples adds `Call sqrt/sin`. Later rungs add op *variants*, never redesign the IR.

**Deliberate scope cuts:** no SSA, no register allocator. A fixed vreg file (16 regs) with last-use freeing — a statement uses ≤ a handful. SSA + a real allocator are the "complexity in core" deferred until a script exhausts registers.

## The assembler (per-ISA, named instructions, label back-patching)

`src/platform/<target>/moonlive_asm_*.{h,cpp}` (behind the platform boundary — it emits ISA bytes). A small `Assembler` that appends instructions to a byte buffer and back-patches label offsets:

- `mov(reg, imm)`, `store8(rbase, roff, rval)`, `add(rd, ra, rb)`, `mul/shl(rd, ra, imm)`, `cmp(ra, rb)`, `branchIf(cond, label)`, `label(l)`, `ret()` — the ~10–15 encodings the IR actually uses, hand-encoded **once per ISA** (vs. once per instruction × per statement template). Label back-patching kills the hand-computed-branch-offset crash class permanently.
- This is the textbook `MacroAssembler` shape (V8 `Assembler`, LLVM `MCInst`, asmjit) — passes *common patterns first*.
- The IR→bytes lowering (`lowerToBytes(const IrProgram&, Assembler&)`) lives per-backend; the IR itself is neutral core.

**The vreg→machine-register + calling-convention contract** between IR and assembler is settled and pinned by a test BEFORE Xtensa: a `Call` surrounded by live vregs must preserve them (which machine regs are caller-saved across the host call, who saves them). Getting this wrong is the multi-round-trip rework *Refactor for simplicity* warns against, so it's nailed on the desktop backend first.

## Files

### Neutral core (`src/core/moonlive/`)
- **`MoonLiveIr.h`** (new) — the IR op structs + `IrProgram` (a flat `std::array`/fixed-capacity list of ops, no heap in the hot build path; sized like `kCodeCap`). Pure data, no ISA.
- **`MoonLiveCompiler.{h,cpp}`** — the parser grows a second production (`setRGB(...)`) and a `random16(...)` primary; codegen becomes **AST → IR** (`lower()`), replacing the direct `emitFill` call. `compileSource` now: parse → lower to IR → hand the IR to the per-ISA `lowerToBytes`.

### Per-ISA assembler + lowering (`src/platform/<target>/`)
- **`moonlive_asm_host.{h,cpp}`** (desktop, arm64 + x86-64 by `#if`), **`moonlive_asm_xtensa.cpp`**, **`moonlive_asm_riscv.cpp`** — the named-instruction assembler + `lowerToBytes` per ISA. The existing `emitFill`/`emitAnimatedFill` stay (the golden fixtures) until the assembler reproduces them, then `emitFill` is re-expressed as `lower(fill-IR) → lowerToBytes` and the hand-blob becomes a test constant.

### Engine + binding (unchanged seam)
- `MoonLive.{h,cpp}` — `compile(source)` already routes through `compileSource`; no change to the engine API. The binding (`MoonLiveEffect.h`) is untouched — the source control now accepts the richer grammar for free.

### Tests
- **`unit_moonlive_ir.cpp`** (new) — AST→IR lowering: `setRGB(5,0,0,255)` produces the expected op list; the `Bounds` node is present before the `Store`; `random16` lowers to a `Call`.
- **`unit_moonlive_asm.cpp`** (new) — the assembler: each named instruction emits the right bytes (golden per ISA); label back-patching resolves a forward/backward branch; the **`Call`-preserves-live-vregs** contract test.
- **`unit_moonlive_compiler.cpp`** (extend) — `setRGB` golden bytes (assembler-built), an out-of-range index is bounds-rejected at runtime (the buffer's other pixels untouched), `random16` index lands in-range, every new parser diagnostic.
- **`unit_moonlive_fill.cpp`** (extend) — **golden regression**: the assembler-built `fill` == the original hand-encoded blob, byte-for-byte (per ISA), proving no codegen-quality regression.
- **`scenario_MoonLiveEffect_livescript.json`** (extend) — a `setRGB(...)` source step + a deliberately out-of-range `setRGB(99999, …)` step (renders safely, no overrun, device keeps ticking) on PC/S3/P4.

## Steps (each independently green)

1. **IR + desktop assembler, `fill` only.** Define the IR; build the host assembler; lower the `fill` IR to bytes; assert byte-identical to the hand blob (golden regression). No language change yet — pure infrastructure swap, proven by the golden test. *The big commit; everything else builds on a proven assembler.*
2. **`setRGB(<lit>, r,g,b)` — desktop.** Parser second production → IR (`Const`+`Bounds`+`Store`) → host bytes. Unit tests: golden bytes, runtime bounds-reject, the cpl-scaling.
3. **`random16(N)` — desktop.** Add the `Call` op + the `random16` built-in (host function) + its lowering; pin the live-vreg-across-Call contract. Now `setRGB(random16(N), blue)` compiles — the hello-world.
4. **Xtensa assembler.** Bring up `moonlive_asm_xtensa`: reproduce the golden `fill`, then `setRGB`/`random16`. Flash S3, run the scenario live.
5. **RISC-V assembler.** Same for the P4. Flash, run the scenario live.
6. **Docs + scenario.** Update `MoonLiveEffect.md` (the IR/assembler pieces, the grammar), extend the scenario, decisions.md (the IR-forces-assembler lesson).

## Validation

- `ctest` + `uv run moondeck/scenario/run_scenario.py` green at each step; desktop-first so steps 1–3 are pure in-process.
- **Golden-bytes regression** (assembler `fill` == hand blob) is the anchor proving the assembler reproduces hand-quality code.
- Hardware: S3 (Xtensa) + P4 (RISC-V) run `setRGB`/`random16` live via the scenario, including the out-of-range-index safety step.
- Build zero-warnings; platform-boundary check (assembler bytes behind `src/platform/`); `check_specs` green.

## Risks / watch-items

- **The assembler is the real cost** — ~10–15 instruction encodings × 3 ISAs, hand-encoded once each, with label back-patching. Sized honestly: a few days per ISA, but each *instruction* is encoded once (not each instruction × statement), and back-patching removes the hand-offset crash class. Desktop-first keeps the feedback loop instant.
- **vreg/calling-convention contract** — the one thing that, gotten wrong, causes multi-round-trip rework. Pinned by a test before Xtensa.
- **No silent scope creep** — no SSA, no register allocator, no float ops, no loops-in-source, no 2D/3D this rung. Each is a later rung. If a step wants one, it's backlogged, not smuggled in.
- **Golden fixtures must not rot** — keep the hand blobs as test constants even after `emitFill` is re-expressed through the assembler, so the regression anchor survives.

## Out of scope (later rungs)
Read-modify-write / trails (stage 2 of the tutorial ladder), oscillators + float codegen (stage 3), 2D/3D addressing (stages 4–5), Ripples graduation, source-level loops, variables, the register allocator, SSA. This plan is the second statement + the IR/assembler it forces.
