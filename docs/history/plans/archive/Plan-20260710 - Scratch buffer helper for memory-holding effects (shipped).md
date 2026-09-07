# Plan — A core scratch-buffer helper for memory-holding effects

## Context

Writing the ["Build your own MoonModules" guide](../../../usecases/build-your-own-moonmodules.md) surfaced a recurring boilerplate across memory-holding effects: allocate a heap buffer in `onBuildState()` (sized to the grid, re-alloc only if the count changed), free it in `teardown()`, free it again in the destructor, hand-write a private `release()` helper, and guard `loop()` with `if (!buf_) return;`. This is the pattern in **~11 effects** (Fire, GameOfLife, GEQ, Tetrix, Particles, StarField, StarSky, BouncingBalls, Solid, NetworkReceive, Wave) — several hold *multiple* buffers (GameOfLife allocates 3 planes, StarSky 5, StarField 3), so the bookkeeping is real. It's textbook *[Complexity lives in core](../../../CLAUDE.md#principles)*: the same non-trivial lifecycle wants to live once in a core primitive, so each effect drops to "declare the buffer, use it."

Three product-owner remarks on the guide drove this (R6 "should `release()` be a hook", R7 "`if (!heat_) return` sounds like orchestration, hide it", and — writing the memory example — "`static_cast` should not be used by module makers"). The answer to all three is the same primitive.

> **Naming note (coordinates with the [hook-rename plan](Plan-20260710%20-%20Rename%20module%20hooks%20to%20prepare-tick-release%20(shipped).md)).** That plan renames the *base lifecycle hooks* — `onBuildState`→`prepare`, `teardown`→`release`, `loop`→`tick`. This plan uses the current names (`onBuildState`/`teardown`) in its before/after so it reads against today's code, but the two are compatible: this plan *removes* the per-effect private `release()` helper and the effect-level `teardown()` entirely, so there is no clash with the base hook becoming `release()`. Whichever ships first, the other adjusts its examples to match; the mechanisms don't collide.

**Explicit design goal — no `static_cast` in effect code.** `platform::alloc()` returns a raw `void*`, so every memory-holding effect today writes `heat_ = static_cast<uint8_t*>(platform::alloc(n))`. That raw cast is exactly the kind of low-level plumbing a domain author should never have to touch. `ScratchBuffer<T>` owns the cast **once, inside the primitive** (typed on `T`), so an effect writes `heat_.resize(n)` — no `static_cast`, no `void*`, no `sizeof`. **Success criterion: after migration, the memory-holding effects contain zero `static_cast` for their scratch buffers** (grep confirms it), and the guide's memory example reads cast-free.

## Prior art (in this repo)

- **`Buffer`** ([Buffer.h](../../../src/light/layers/Buffer.h)) — the LED **pixel** buffer: `allocate(nrOfLights, cpl)` / `free()` / `clear()`, RAII-ish (frees in its own path), uses `platform::alloc`/`free`. The scratch buffer is its **sibling for effect *state*** — same allocate/free discipline, but a typed array of arbitrary element type (a `uint8_t` heat value, a packed cell bit-plane, a `Particle` struct), not fixed at 3-bytes-per-light.
- **`std::unique_ptr` with a custom deleter** is the C++ standard answer, but the codebase uses `platform::alloc`/`free` (not `new`/`delete`) for PSRAM/DMA placement, so a small owned type wrapping those is the recognizable, house-consistent shape — the same call `Buffer` already makes.

## Design

### Flash-bloat concern (product-owner remark) — and the fix

A naive `template <class T> class ScratchBuffer` where **every** method is templated *would* bloat flash: the compiler emits a full copy of `resize`/`data`/etc. per distinct `T`. But the instantiation reality caps the risk — of the ~19 scratch buffers, **14 are `uint8_t`** (one shared instantiation, zero duplication), plus `nrOfLightsType`/`lengthType` (both integer) and 3 one-off struct types (`Ball`, `Star`, `Tetris`). So the worst case is ~5 instantiations of *tiny* methods.

The design **removes even that** with the standard "type-erased base + thin typed façade" split (the same trick `std::vector` implementations use to share code across element types):

```cpp
// core/ScratchBuffer.h

// NON-template base: all the real logic (alloc/free/resize/zero + the owner tie)
// compiled ONCE, in bytes — no per-T duplication. This is where the flash goes.
class ScratchBufferBase {
protected:
    explicit ScratchBufferBase(MoonModule& owner);   // register with the module (.cpp)
    ~ScratchBufferBase();                            // free + deregister (.cpp)

    // Size to `bytes` (0 frees), reallocating only if the byte count changed. Zero-fills
    // on (re)alloc. Owns the one platform::alloc + the raw void*, so no caller ever casts.
    // Updates owner_'s dynamic-bytes total by the delta — so the UI readout self-maintains.
    bool resizeBytes(size_t bytes);                  // .cpp — the only heavy body

    MoonModule& owner_;
    void*  raw_  = nullptr;
    size_t bytes_ = 0;
    // move-only; copy deleted.
};

// TEMPLATE façade: pure type-safe sugar over the base. Every method is a one-line
// inline that forwards to the base — inlines to nothing, so a new T adds ~0 flash.
template <class T>
class ScratchBuffer : private ScratchBufferBase {
public:
    explicit ScratchBuffer(MoonModule& owner) : ScratchBufferBase(owner) {}
    bool   resize(size_t count) { return resizeBytes(count * sizeof(T)); }
    T*     data()        { return static_cast<T*>(raw_); }   // the ONE cast, hidden here
    size_t count() const { return bytes_ / sizeof(T); }
    size_t bytes() const { return bytes_; }
    explicit operator bool() const { return raw_ != nullptr; }
    T&     operator[](size_t i) { return data()[i]; }
};
```

The **owner tie** (`ScratchBufferBase(MoonModule&)`) is the one non-obvious piece and it earns its place: it makes both `setDynamicBytes` *and* disable-free automatic. On `resizeBytes`, the base adjusts `owner_`'s dynamic-bytes total by the size delta (UI readout self-maintains). On disable, `EffectBase::teardown()` walks the buffers each module registered and frees them (or, simpler, each buffer's destructor already frees, and `teardown` resizes them to 0 through the registration list — resolve the exact mechanism in review, decision #1). Either way the *effect* writes neither `setDynamicBytes` nor a buffer-freeing `teardown`.

**Flash cost:** the allocate/free/resize/memset logic is compiled exactly once (`ScratchBufferBase::resizeBytes` in a `.cpp`). Each `ScratchBuffer<T>` adds only trivially-inlinable forwarders — the compiler folds them into the call site, emitting no separate function bodies. So a device with `ScratchBuffer<uint8_t>` + `ScratchBuffer<Ball>` + `ScratchBuffer<Star>` pays for the shared base **once**, not three times. This is strictly *less* flash than the ~11 effects' hand-written `alloc`/`free`/`release` today (that logic is currently duplicated per effect).

An effect sees only the façade — `heat_.resize(n)`, `heat_[i]`, `if (heat_)` — with the raw `void*` and the one `static_cast` sealed inside the base (**the "no `static_cast` for module makers" goal**).

That is the entire primitive — it *is* the `release()` helper (the destructor + `resize(0)`), and it *is* the null-guard (`operator bool` / `data()==nullptr`), so all three remarks dissolve.

> **Verification the plan must include:** measure `.text`/flash before and after the migration on an ESP32 build (the KPI gate already reports image size). Expectation: flash **shrinks** (11 effects' duplicated alloc/free logic collapses to one shared base) or stays flat — never grows. If a naive all-template version were used instead, this is the number that would catch the bloat; the base/façade split is what keeps it down.

### What an effect becomes

Before (the guide's `SparkleEffect`, ~15 lines of bookkeeping):

```cpp
void onBuildState() override {
    nrOfLightsType n = nrOfLights();
    if (n != heatCount_) { release(); heat_ = (uint8_t*)platform::alloc(n); heatCount_ = heat_ ? n : 0; }
    setDynamicBytes(heatCount_);
}
void teardown() override { release(); setDynamicBytes(0); }
~SparkleEffect() override { release(); }
void loop() override { if (!heat_) return; /* … */ }
void release() { platform::free(heat_); heat_ = nullptr; heatCount_ = 0; }
uint8_t* heat_ = nullptr; nrOfLightsType heatCount_ = 0;
```

After — the goal is **declare a buffer, use it**, with *every* line of bookkeeping gone:

```cpp
class SparkleEffect : public EffectBase {
public:
    void onBuildState() override { heat_.resize(nrOfLights()); }   // that's the whole hook
    void loop() override {
        if (!heat_) return;                 // one honest line — the 0×0-grid / alloc-failed case
        for (size_t i = 0; i < heat_.count(); i++) heat_[i] = /* … */;
    }
    ScratchBuffer<uint8_t> heat_{*this};    // *this ties it to the module (see below)
};
```

The `release()` helper, the `teardown()`, the destructor, the `setDynamicBytes` calls, the `heatCount_` mirror, and the `static_cast` **all disappear**. The four things the product owner flagged as orchestration are each removed by the primitive, not just relocated:

| Line the author writes today | Why it's ceremony | In the `ScratchBuffer` design |
|---|---|---|
| `setDynamicBytes(heat_.bytes())` | The buffer already knows its own byte count; the module is mirroring a value core can read from the buffer | **Gone.** The buffer is constructed with a reference to its owning module (`ScratchBuffer<uint8_t> heat_{*this}`) and adds/subtracts its bytes from the module's dynamic-bytes total on every `resize` — so the UI readout stays correct with no line in the effect. |
| `teardown()` **and** `release()` (one is redundant) | Two names for "free the buffer" | **Both gone.** The buffer frees itself; there is no `release()` and no effect-level `teardown()` for the buffer (see disable-free below). |
| `~SparkleEffect() { release(); } // just in case` | "just in case" is a code smell — either it's needed or it isn't | **Gone.** The member's own destructor frees it deterministically — not "just in case," but *by RAII*, the one correct place. |
| `if (!heat_) return;` | Looks like an enabled-check | **Kept, and correct** (it reads `if (!heat_)` via `operator bool`). It answers *"does my buffer exist?"* (0×0 grid, alloc-failed) — a real question the core doesn't answer for you, distinct from *"am I enabled?"* (which the core does answer, by not calling `loop()`). This one is not ceremony; it's a genuine guard, and it's the only survivor. |

**Disable-free — solved by the owner tie, no per-effect `teardown()`.** The buffer must free when the effect is *disabled* (not just destroyed). Because the buffer holds a reference to its module, `EffectBase::teardown()` (the base, called by `applyState()` on disable) frees every `ScratchBuffer` the module registered with it — one place in the base, zero lines per effect. (This replaces the earlier "each effect keeps a one-line `teardown()`" idea: the owner reference the buffer needs *anyway* for `setDynamicBytes` is exactly the handle the base needs to free it, so both fall out of the same tie.)

## Open decisions (resolve in review, before coding)

1. **The owner tie — reference vs. registration.** `ScratchBuffer<T> heat_{*this}` gives the buffer a `MoonModule&` so it can (a) keep the module's dynamic-bytes total current and (b) let the base free it on disable. Confirm the ergonomics: the `{*this}` in the member declaration is the one bit of "wiring" the author writes — weigh it against the ~15 lines it removes (clearly worth it), and confirm it composes for a module with several buffers (each registers itself). The alternative — a `std::unique_ptr`-style buffer with no owner + a per-effect one-line `teardown`/`setDynamicBytes` — is simpler in the primitive but pushes two lines back onto every effect; the owner tie is the one that delivers "declare and use."
2. **Multi-buffer effects (GameOfLife: 3, StarSky: 5).** Declare N `ScratchBuffer` members, each `{*this}`; each resizes and reports independently. The win scales (N destructors + N release-lines + N setDynamicBytes collapse). Verify the worst case (GameOfLife) reads cleanly before rolling to all.
3. **Element types.** `uint8_t` (heat, cells, packed bit-planes sized in bytes) and struct arrays (`ScratchBuffer<Ball>`). Confirm the base/façade split handles all — it does: the base is byte-oriented, the façade multiplies by `sizeof(T)`.

## Files

- **New:** `src/core/ScratchBuffer.h` (the template) + `test/unit/core/unit_ScratchBuffer.cpp` (resize grows/shrinks/frees, `operator bool`, zero-fill, move semantics, resize(0) frees).
- **Migrate (one at a time, each its own diff):** the ~11 effects above — replace the raw pointer + `release()` + destructor + manual null-guard with a `ScratchBuffer` member. Start with the simplest (Fire, one buffer) to pin the pattern, end with the worst (GameOfLife, StarSky) to prove it scales.
- **Docs:** once it lands, simplify the guide's "When you need memory" section to the `ScratchBuffer` form (the guide already flags "this boilerplate may get shorter" — deliver on it).

## Verification

1. `cmake --build build` clean; `ctest` + scenarios green (the memory scenarios — GameOfLife, GEQ — exercise exactly this alloc/free path per grid change).
2. **No behaviour change:** each migrated effect renders identically; `dynamicBytes` per effect unchanged (the UI card readout is the observable).
3. **The disable cascade still frees:** the existing `unit_Layers_container` cascade test (disable a parent Layer → child effect's `dynamicBytes → 0`) must still pass — it's the guard that a `ScratchBuffer` frees on the disable path.
4. ASAN clean across a resize/disable/re-enable cycle (no leak, no use-after-free) — the primitive's whole point.
5. **Flash does not grow (the bloat check):** compare the ESP32 image size (KPI gate reports it) before vs. after migration. The base/façade split means the shared logic is compiled once; expectation is flat-or-smaller flash. A regression here means the type-erased base didn't work as intended (e.g. a method that should be a one-line forwarder grew a body) — fix the split, don't accept the growth.
6. **Zero `static_cast` for scratch buffers:** grep the migrated effects — the raw cast lives only inside `ScratchBufferBase`, nowhere in effect code.

## Scope guard

One core primitive + mechanical per-effect migration. Do **not** fold in the pixel `Buffer` (it's a different, fixed-shape type with its own `clear`/`channelsPerLight` semantics — leave it). The `setDynamicBytes` and disable-free automation ride on the **one** owner-reference the buffer already needs — don't invent a *separate* registration mechanism for each. Ship the primitive + Fire migration first as proof (confirm flash flat-or-smaller), then the rest.
