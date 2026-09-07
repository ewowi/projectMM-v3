# Plan — MoonLive Stage 0: native-codegen load-bearing spike

> Approved plan record (CLAUDE.md *Plan before implementing*). Implements the first, smallest step of [livescripts-analysis-top-down.md](../../../backlog/livescripts-analysis-top-down.md) — its Stage 0 "load-bearing spike", split one notch finer so the single novel hardware risk is isolated and proven before any compiler front-end is written. S3-only, bare-minimum assembler, near-zero language.

## Goal

Prove the one link nothing else can de-risk: **text-authored intent → native machine code we generated → `allocExec` executable memory → called every render tick → it writes the real producer buffer → visible on the S3, no crash.** Everything above that link (tokenizer, parser, IR, real codegen) is conventional, desktop-testable compiler work; this spike attacks only the part that can't be tested off-hardware.

Visual acceptance: add a scripted effect on the S3 → the grid lights solid blue; then a per-frame hue sweep. Checkable by eye in the preview.

## Why split Stage 0 into 1a/1b/2

The analysis doc's Stage 0 bundles two risks of very different character into one increment:
- **The novel, hardware-only risk:** emit Xtensa bytes → IRAM `allocExec` → cache-sync → call via the windowed-register ABI → write `buffer()`. Nothing in projectMM does this yet.
- **The conventional, low-risk, desktop-testable risk:** a hand-written recursive-descent front-end (lex → parse → emit) for one statement.

Bundling them means a first-pixel failure is ambiguous ("bad codegen, or bad allocExec?"). So:

- **1a/1b = the load-bearing spike** — hand-emit the bytes (no language at all), prove the scary link. The hand-emitted bytes are not throwaway: they become the **golden reference** the real codegen (step 2) must reproduce.
- **2 = the genuine Stage-0 vertical slice** — replace the hand-emitted array with a minimal real `lex("fill(blue)") → parse → emit the SAME bytes`, reusing the exact `allocExec` + binding + buffer surface 1a proved.

This is "small in depth AND broad": depth = one statement; broad = the whole vertical (binding → allocExec → call → buffer), each piece minimal.

## Decisions locked with the product owner

- **First commit = hand-emitted bytes (Stage −1), then grow into the doc's Stage 0** (a tiny real front-end). Not one-shot Stage 0 — the two risks are separated so a first-pixel failure is unambiguous, and 1a is the test oracle for 2.
- **Solid color first (1a), then per-frame hue (1b)** — a static fill answers "does our native code run and write the buffer" unambiguously; passing `elapsed()` then proves dynamic input reaches native code, a distinct fact worth isolating.
- **S3 only.** Xtensa backend + the desktop x86-64 backend (needed anyway for in-process unit tests); no other ISA this spike.
- **Aligns with the precompiled-effect surface.** The emitted function uses the exact `buffer()` + `nrOfLights()*channelsPerLight()` raw-write surface a compiled effect uses today, so swapping hand-bytes → codegen later changes nothing host-side. If the producer-buffer set/get surface wants to change (the RGB-into-buffer question the product owner flagged), this spike is the cheapest place to discover it.

## Architecture placement (respecting the boundaries)

Per [§3.9](../../../backlog/livescripts-analysis-top-down.md) (domain-neutral engine core, thin binding) and the **platform boundary** hard rule (ISA codegen lives only in `src/platform/<target>/`):

```
src/platform/platform.h                     ← + allocExec/freeExec seam (declaration only)
src/platform/esp32/platform_esp32.cpp       ← S3: heap_caps_malloc(MALLOC_CAP_EXEC) IRAM + cache sync
src/platform/desktop/platform_desktop.cpp   ← desktop: mmap(PROT_READ|WRITE|EXEC)
src/core/moonlive/MoonLive.h                ← neutral engine: holds an exec block, run(buf,n,cpl); compile() stubbed
  (the hand-emitted byte arrays are ISA-specific → they live behind the platform line,
   emitted by a tiny per-ISA function the engine calls; the engine itself stays neutral)
src/platform/esp32/moonlive_emit_xtensa.*   ← the ~15 hand-coded Xtensa bytes (step 1) → real emit (step 2)
src/platform/desktop/moonlive_emit_host.*   ← the x86-64 equivalent (so unit tests run in-process)
src/light/moonlive/MoonLiveEffect.h         ← thin EffectBase binding; loop() = engine_.run(buffer(), nrOfLights(), channelsPerLight())
docs/moonmodules/core/MoonLive.md           ← spec (required: every new module .h needs one; check_specs enforces)
```

The engine (`src/core/moonlive/`) never sees `EffectBase`/`Buffer`/`ModuleRole`; the binding (`src/light/moonlive/`) translates. The engine reaches native code only through `platform::allocExec` + a per-ISA `emit*()` the platform layer provides. Dependency direction one-way: binding → engine → platform seam.

## Steps

### Step 1a — `allocExec` + hand-emitted solid-fill, called over the buffer

**New platform seam** (`platform.h` + both backends):
```cpp
void* allocExec(size_t bytes);   // executable memory; nullptr on failure (degrade, never crash)
void  freeExec(void* ptr, size_t bytes);
```
- **S3:** `heap_caps_malloc(bytes, MALLOC_CAP_EXEC)` (IRAM); after copying code in, flush/invalidate so the I-cache sees fresh bytes (the Xtensa cache-coherency step — the real unknown). Return nullptr if IRAM is exhausted.
- **Desktop:** `mmap(NULL, bytes, PROT_READ|PROT_WRITE|PROT_EXEC, MAP_PRIVATE|MAP_ANON, -1, 0)`; `munmap` in freeExec.

**Engine (`MoonLive.h`, neutral):**
```cpp
class MoonLive {
public:
    bool compile();              // step 1: calls platform emit*() → fills an allocExec block. true on success.
    void run(uint8_t* buf, nrOfLightsType n, uint8_t cpl);  // calls the block as a fn ptr
    void free();
    bool ok() const; const char* error() const;
};
```
`run` casts the exec block to `void(*)(uint8_t*, uint32_t, uint8_t)` and calls it. The emitted function's contract: fill `n*cpl` bytes of `buf` with a fixed BGR/RGB pattern, return. ~10–15 Xtensa instructions, hand-encoded with a comment naming each (`entry`, the loop, `s8i` store, `addi`, `bne`, `retw`). Desktop emit: the equivalent x86-64 (or, honestly, a C function whose address we hand back — the desktop path's job is to test the *binding + engine API*, not ISA encoding; the real ISA test is the Xtensa hardware run).

**Binding (`MoonLiveEffect.h`):** a normal `EffectBase`; `setup()`→`engine_.compile()`; `loop()`→ `if (engine_.ok()) engine_.run(buffer(), nrOfLights(), channelsPerLight())`; `teardown()`→`engine_.free()`. Register in `main.cpp` + `scenario_runner.cpp`.

**Acceptance:** desktop unit test — compile, run over a known buffer, assert every light == the fill color. On the **S3**: add `MoonLiveEffect` to a Layer (via API), grid lights **solid blue**, fps stable, no crash, survives add/delete/replace.

### Step 1b — per-frame hue (dynamic input reaches native code)

Extend `run` to pass `elapsed()`; the emitted code derives a hue from it (simplest: write `elapsed()>>k` into the R channel, or call a host `hsv8(hue)` built-in — pick the cheaper to hand-encode). Proves a host-supplied per-frame value flows into the native code and changes the output.

**Acceptance:** S3 grid **hue/brightness sweeps** per frame, smoothly, within budget.

### Step 2 — a minimal real front-end emits the same bytes

A bare recursive-descent slice in `src/core/moonlive/` (neutral): tokenize → parse **one statement** (`fill(blue);` or `fill(<int>);`) → the Xtensa/host emitter produces the **same** bytes step 1a hand-wrote. Still no `.ml` file (source is a hardcoded string or a `source` text control on the effect — TBD in the step, the file system + editor are explicitly later per the analysis doc). No IR yet — direct AST→emit is fine at one statement; the IR seam is introduced when a second statement/type forces it (a later stage), per *concrete-first*.

**Acceptance:** the byte output of `compile("fill(blue)")` **equals** the step-1a golden array (a desktop unit test diffs them); the S3 still lights blue, now from parsed source. This is the no-language-leak proof at its cheapest.

## Tests (every increment is a tested increment — §6 of the analysis)

- `test/unit/core/unit_moonlive_emit.cpp` — desktop: `compile()` produces a non-empty exec block; `run()` over a known buffer yields the exact fill (golden buffer). Step 2 adds: parsed-source bytes == hand-emitted golden bytes.
- `test/unit/core/unit_platform_allocexec.cpp` — `allocExec` returns executable, writable memory; a trivial emitted "return 42" function called through it returns 42 (desktop); nullptr-on-exhaustion path degrades.
- `test/scenarios/light/scenario_moonlive_hello.json` — wire `MoonLiveEffect` as a real MoonModule: add, measure (buffer non-zero == fill color), delete, re-add (robustness), at a couple grid sizes incl 0×0×0 (no crash). Runs in-process (desktop backend) on every commit; runs live on the S3 over REST for the ISA backend.
- Robustness: add/delete/replace the scripted effect in any order alongside compiled effects — the hard rule.

## Validation

- `ctest` + `uv run moondeck/scenario/run_scenario.py` green at each step.
- **The hardware acceptance is the point:** S3 solid blue (1a), S3 hue sweep (1b), S3 blue-from-source (2) — eyeballed in the preview, the product owner confirms.
- Desktop build zero-warnings (`-Wall -Wextra -Werror`); platform-boundary check passes (all ISA bytes behind `src/platform/`).
- `check_specs.py` green — `docs/moonmodules/core/MoonLive.md` written (it is the module's home; carries the neutral engine API, the allocExec contract, the §3.9 boundary, and the ESPLiveScript/ARTI-FX/MoonLight prior-art block staged in the analysis doc).

## Risks / watch-items

- **Xtensa cache coherency after writing IRAM** — the genuine unknown. If the called bytes execute stale, the fill won't show; the fix is the correct flush/invalidate around the copy (Espressif's `MALLOC_CAP_EXEC` + cache-sync pattern). This is *why* 1a is hand-emitted: 15 known-correct bytes make this the only variable.
- **Windowed-register call ABI (`entry`/`retw`)** — the emitted function must open/close a register window correctly or the call corrupts the stack. Hand-encoding it first means the ABI is debugged in isolation.
- **IRAM scarcity on the S3** — exec blocks compete with WiFi/driver IRAM; `allocExec` must degrade (nullptr → effect reports "no memory" status, keeps running dark) not crash. Pin it in the allocExec test.
- **Desktop backend honesty** — the desktop path tests the binding/engine API and the front-end, NOT Xtensa encoding. The plan states this so the desktop green is never mistaken for ISA validation; the S3 run is the real gate.
- **Scope creep** — no IR, no file system, no editor, no second statement, no controls in this spike. Each is a later ladder rung. If a step wants one of those, it's out of scope and gets backlogged, not smuggled in.

## Out of scope (explicit — later rungs)

IR seam (introduced when a 2nd statement/type forces it), `.ml` files + file manager + editor window, the second ISA seam-proof (analysis Stage 0.5 — done after the front-end exists, not at hello-world), controls binding (Stage 1), math/time/2D/3D, RipplesEffect graduation. This plan is Stage 0 only.
