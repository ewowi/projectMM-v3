# Plan — Release 4 scope: effect breadth + the rename runway

## Status — SHIPPED 2026-09-07 (decomposed)

**The headline SHIPPED and was overachieved; the driver work never started.** Verified against the
tree rather than inferred: this plan asked for Stage-1 primitives plus "the next effect batch" to
move the rename's breadth gate, and the library is now **66 compiled effects and 32 scripted**
against the ~21 this document counted. The gate it existed to serve is met; the remaining blocker
for the rename is DMX, not effects ([the migration plan's status](../Plan-20260630%20-%20MoonLight%20migration%20%28multi-stage%29.md)).

| item | state |
|---|---|
| Stage-1 primitives (palette, draw, FastLED-named set, tags) | **shipped** |
| The next effect batch | **shipped, overachieved**: 98 effects against a 21 baseline |
| `ActiveInstance` primitive | **shipped** (`src/core/ActiveInstance.h`, used by AudioService + DevicesModule) |
| CodeRabbit #29, the scenario finding | **shipped 2026-09-07**, see below |
| CodeRabbit #29, the other three | **open**, moved to backlog |
| RS-485 / DMX-512 driver | **not started**, and scoped OUT by the product owner 2026-09-07 |
| High-light-count driver work (4 items) | **not started**, moved to backlog |

**SHIPPED as a release-scope plan.** Its headline landed and was overachieved, which is what this
document existed to do. What did not land was never work this plan owned: each item was a pointer
to something with its own home, and each is still there.

- **DMX** is Stage 5 of the [MoonLight migration plan](../Plan-20260630%20-%20MoonLight%20migration%20%28multi-stage%29.md),
  which describes it in far more detail than this plan ever did, and where it belongs: it is the
  transport half of the moving-head work whose effect half already shipped.
- **The high-light-count drivers** and the **CodeRabbit #29 findings** stay in the backlog. Neither
  belongs to the migration plan, which is about porting MoonLight's library, not about lane drivers
  or a core/platform boundary.

Nothing is orphaned by closing this, which is the test for whether a plan can be closed at all.

### The one thing fixed while decomposing

The CodeRabbit #29 **scenario finding** is closed: `scenario_modifier_chain` routed a
modifier-composition test through `NetworkSendDriver`, pulling socket behavior into a test that is
not about the network. It now uses `PreviewDriver`, the in-process sink (the shape
`scenario_Audio_mutation` already used).

Its `tick_us` half was NOT a defect and is left alone: a `measure` step asserts nothing, it RECORDS,
and that recording is what feeds the per-commit performance trend (CLAUDE.md, "scenarios record").
Removing it would have blinded the trend to fix nothing.

## Context

Release 3 is being cut now. This plan captures the **Release 4** candidates — the next strategic thread after R3 — so the direction is recorded before the work starts. The product owner's steer: the items below are R4, not R3.

The backlog has one dominant strategic thread that most other items orbit: the **projectMM → MoonLight rename** ([backlog rename plan](../../../backlog/rename-to-moonlight.md)). Its gate is *"the effect library must not feel thin next to the predecessor's 60+ effects."* Two in-flight plans feed that gate, and R4 is where they land. The shape of R4 is therefore **"the effects release + the rename runway"**: grow visible feature breadth while moving the single most important strategic gate (rename readiness), and leave the hardware-verification-bound driver work to its own dedicated push.

This is a roadmap/scope plan, not a single-feature `/plan`. Each item below gets its own `/plan` + commit when reached; this document is the *map* and the *why*.

## The spine — effect-breadth parity (headline)

**MoonLight migration, Stage 1 + the next effect batch.** ([Plan-20260630 - MoonLight migration (multi-stage)](../Plan-20260630%20-%20MoonLight%20migration%20%28multi-stage%29.md).)

This is the biggest lever and the explicit *"execution vehicle for the effect-breadth parity gate."* ~21 of the predecessor's 60+ effects are ported. Stage 1's prerequisites are the highest-value core work available, because every future effect leans on them:

- **Shared palette** — hard prerequisite; many effects color via `ColorFromPalette`. Generalize the pattern `PlasmaPaletteEffect` hard-codes today.
- **The shared primitive library** — FastLED-named, our own implementation, hot-path-tuned integer-only: `beatsin8`, `inoise8`, `qadd8`, `nscale8`, `random8`/`random16`, `ColorFromPalette`, and the dimension-agnostic draw set. Extends the existing `color.h` (`scale8`, `sin8`).
- **Tag/emoji legend** — settle before batch-migrating so every module is consistent from batch one.
- **Per-library doc model** — `effects_<library>.md` compact table rows (per [ADR 0015](../../../adr/0015-library-is-a-tag-not-a-folder.md)); changes the `check_specs.py` contract.

Then the next migration batch on top. This is the R4 headline: it unblocks the rename *and* is pure user-visible feature growth.

## Two quick wins — scoped and ready

- **Active-instance election primitive.** ([Plan-20260710 - Active-instance election primitive](Plan-20260710%20-%20Active-instance%20election%20primitive%20%28shipped%29.md).) A core `ActiveInstance<T>` that removes duplicated singleton-election bookkeeping from `AudioService` + `DevicesModule` (both had real dangling-static bugs). Textbook *Complexity-lives-in-core* subtraction; small; in flight.
- **CodeRabbit #29 boundary findings (4).** ([backlog-core § MoonLive core/platform layering](../../../backlog/backlog-core.md#moonlive-coreplatform-layering-jit-sdkconfig-scoping-coderabbit-29-3-findings-left).) MoonLive core-includes-platform + compiled-into-`mm_core`, W^X disabled in the board default, a scenario riding timing + network. Real, already scoped; good hygiene to close before a named release.

## What did not happen, and where it lives now

Each of these was a POINTER to a backlog item rather than work this plan owned, and each is still
there under its own name. Listed here only so the decomposition is traceable; the backlog is the
one home for what they are and why.

- **RS-485 / DMX-512 wired output** moved to the [MoonLight migration plan's Stage 5](../Plan-20260630%20-%20MoonLight%20migration%20%28multi-stage%29.md),
  which is its real home: the moving-head EFFECTS shipped there, and this is their transport.
  Scoped OUT by the product owner on 2026-09-07 ("out of scope for now, will do later").
- **Classic-ESP32 shift-register ring on raw I2S**, **P4 Parlio streaming ring**, **shared
  lane-driver scaffolding** ([backlog-light § Drivers](../../../backlog/backlog-light.md#drivers)) and
  the **MoonI80 prime-only ring stall backstop**
  ([backlog-core](../../../backlog/backlog-core.md#mooni80-prime-only-ring-no-stall-backstop-sibling-path-gap)).
  All four are hardware-verification-bound: each needs the expander wall and the relevant board, so
  they land with bench sign-off or not at all.
- **CodeRabbit #29, three findings**
  ([backlog-core](../../../backlog/backlog-core.md#moonlive-coreplatform-layering-jit-sdkconfig-scoping-coderabbit-29-3-findings-left)).
  The fourth is closed, above.

## Success shape (as written in July, and what became of it)

> R4 ships when: the migration Stage-1 primitives + the next effect batch have landed (moving the
> rename's breadth gate forward), the `ActiveInstance` primitive and the CodeRabbit #29 boundary
> fixes are in, the RS-485/DMX driver reaches a verified first output, and the high-light-count
> driver work above is bench-verified.

The first half happened and then some. The second half did not, and DMX is now deferred, so this
shape is unmeetable as stated: kept verbatim because a scope that was written down and then overtaken
is worth reading next to what actually shipped, not quietly rewritten to match the outcome.

The lesson worth carrying, and the reason this document is decomposed rather than extended: it
bundled **effect work that needed only a keyboard** with **driver work that needs a wall of LEDs**.
The first raced ahead; the second never started, because it was gated on bench time rather than on
anything this plan could schedule. A release scope that mixes the two makes neither legible. Split
by what a task is BLOCKED ON, not by which release it is wanted for.
