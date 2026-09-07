# Plan: `///` doc-comment size reporting via clang-query

## Context

The project publishes `///` doc comments through moxydoc: they sit on classes, methods and
attributes and become the module catalog pages. Nothing measures them. Some have grown past the
point of being read — `MoonLedDriver.h` carries a **9731-character, 129-line** class comment —
while others are a single word. Growth is invisible until someone reads the published page.

The PO asked for a comment sanity check with the threshold decided *after* seeing real counts.
This plan builds the report and deliberately ships it with **no threshold**, the same sequence
that made the array rule usable: thresholds come from the measured distribution, not from
guessing in advance.

**Scope (PO):** `///` only, via the AST — `clang-query` stays purely AST-driven.

## Why `///` is in the AST and `//` is not

A compiler decision, not a quirk. `///` and `/** */` are *documentation* comments: Clang parses
them into `FullComment` / `ParagraphComment` / `TextComment` nodes and **attaches each to the
declaration that follows**, because the compiler consumes them (`-Wdocumentation` validates
`@param` names against real parameters; IDEs show them on hover). `//` is an ordinary comment
with no declaration to bind to — it can sit mid-expression — so the lexer discards it.

That attachment is what makes this rule worth building: every finding arrives with its
declaration and scope, which is what allows per-scope thresholds.

**Probed and confirmed** on this tree (Homebrew clang-query 22, `-p build`):
- `cxxRecordDecl(hasName("MoonI80Peripheral"))` dumps `FullComment <line:10:4, line:138:104>` —
  the full span, so length is measurable straight from the range.
- `cxxMethodDecl` and `fieldDecl` each dump their own attached `TextComment` nodes, so the three
  scopes separate cleanly. One `TextComment` per source line, `Text=" …"`, so chars are summable.

## `//` comments are already covered — by lizard

Recorded because I got this wrong twice while planning. I first said lizard does not check
comments at all; the PO pointed at the `LINES` column, and the measurement backs them up:

- `LINES − NLOC` is comment-and-blank-aware — our own MoonDeck.md says "`LINES` minus `NLOC` is
  roughly how much of the function is documentation".
- **582 of 2129** functions have a non-zero gap. Median 5, p90 24, **max 217** (`mm_main`).
- **94%** of all `//` lines in `src/` (16211 of 17211) sit inside a function body, where the gap
  sees them. Only 6% — file headers, comments between declarations — are invisible to it.

Two honest limits: the gap conflates comments with blank lines, and it is per *function*, not per
comment block. Fine for ranking "which function is drowning in prose"; not a per-comment
character count. **No new `//` tooling is needed** — the signal exists and is already collected.
What is missing is that nothing *reports* on it, which is a one-line follow-up (surface
`LINES − NLOC` in the lizard table's summary), not a new tool.

## Measured `///` distribution — the reason for per-scope thresholds

619 blocks in `src/`, by what they attach to:

| scope | n | median | p75 | p90 | p95 | max |
|---|---|---|---|---|---|---|
| class | 114 | 83 | 1629 | 3748 | 4389 | **9520** |
| method | 400 | 235 | 378 | 607 | 835 | 2952 |
| field | 63 | 447 | 765 | 1644 | 1706 | 3529 |

Lines: class median 2 / max 129; method median 3 / max 40; field median 6 / max 51.

The class row is **bimodal** — a mass of one-liners plus the module-spec headers
(`MoonLedDriver`, `HttpServerModule`, `NetworkModule`, `MqttModule`). One flat threshold would
flag the best-documented modules as defects, repeating the `MoonModule::name_[16]` trap where a
past win reads as a problem. Method comments are tight and consistent — that is where a threshold
will bite usefully. Hence: **no threshold at first, split by scope, decide from the card output.**

## Design

One new rule in `check_clang_query.py` — no new script, no new card. That script is the
designated home for bespoke AST rules and already owns everything this needs.

**Reuse, do not reinvent** (`moondeck/check/check_clang_query.py` unless noted):
- `_run_rule()` (:233) — writes the query file, invokes clang-query per TU, applies
  `check_clang_tidy._toolchain_args()` (`check_clang_tidy.py:71`) for `-isysroot`. Without it the
  run silently under-reports (measured: 129/129 files errored); that trap is already solved here.
- `_source_tus()` (:223) — the ~15 TUs from `compile_commands.json`.
- `_rel()` (:254) — repo-relative path, `None` for SDK/vendored: the our-files filter.
- `_truncate()` (:262) — `--max-rows` plus the announced "N more not shown" line.
- The silent-zero guard (:533) — bails when clang-query errored instead of reporting a clean zero.
- `module_files()` / `including_tus()` (:169/:182) — `--module` scoping.

**The rule**, added to `RULES` (:129), matching the three decl kinds and filtering in Python:
```
"comments": { "title": "Doc comment size (///)", "output": "dump",
              "matcher": "namedDecl(anyOf(cxxRecordDecl(), cxxMethodDecl(), fieldDecl()))" }
```
Detect `FullComment` in the dump and sum its `TextComment` lines — the same "match broadly, filter
in Python" shape the array rule already uses because no size matcher exists.

**Honest cost.** The script's docstring claims a rule is one dict entry; it is not. Dispatch is
hardcoded in two if/elif chains in `main()` (:540, :548), so this needs a `collect_comments()`, a
`render_comments()` and a regex — realistically **40–70 lines**. A fourth rule name falling
through the `else` branches lands silently in `collect_heap`/`render_heap`, so both chains must be
wired explicitly. Converting dispatch to a table lookup is worth it **only if** it stays a few
lines — otherwise it is scope creep on a report.

**Output**: one table sorted worst-first — `CHARS · LINES · SCOPE · NAME · FILE:LINE` — plus a
per-scope summary (count / median / max) so the threshold conversation has its numbers present.
Matches existing tables (data-derived widths, capped, 2-space indent).

## Files

- `moondeck/check/check_clang_query.py` — the rule, collector, renderer, both dispatch chains.
- `moondeck/MoonDeck.md` — extend `### check_clang_query` (:250) with a `Rule comments`
  subsection: what it reports, the per-scope distribution, why no threshold yet.

No `moondeck_config.json` change: the card exists and gains a rule. No backlog card for `//` —
lizard covers 94% of it already.

## Verification

1. ✅ The rule runs and prints a per-scope table.
2. ✅ **Control check fired.** `HttpServerModule.h` at 7968/101 and `MoonI80Peripheral` at
   9334/129 are both in the report. Four parsing bugs were caught by exactly this step: every
   NAME read as `col`; the largest comment in the tree dropped by a fixed 80-line lookahead;
   ~45 one-line `///` blocks missed because a same-line span prints `<col:23, col:71>` with no
   line number; and 365 of 474 rows mis-attributed until the matcher bound the declaration.
3. ✅ Cross-checked. **629 found** (class 124 / method 400 / field 105) against 642 in source —
   the remainder is 4 blocks in `ImprovFrame.h` plus enum/typedef scopes outside the matcher.
4. ✅ Run through the MoonDeck card; the log reads 629.
5. ✅ 876 unit tests, 19 scenarios, specs clean, three ESP32 firmwares byte-identical.
6. ◻ **OPEN — the reason this plan is not yet realized.** Read the output together and set the
   per-scope thresholds. The report deliberately ships with none.

### Where the thresholds landed

The report went through three shapes on PO direction, each discarded for a measured reason:

1. **Per comment, by characters** — the biggest single comments. Covered only `///`, so it saw
   24% of the tree's comment lines, and could not tell "one huge comment" from "a file of many".
2. **Per file, by line ratio** — hid the outlier inside an average (a 16-line comment in a 27-line
   header is fine), and the PO's "2× the line count" budget could not apply: `/// lines ÷ total
   lines` is capped at 1.0 by construction, so 2.0 would flag nothing.
3. **Per declaration, by words** — what shipped. A line is a formatting accident; words are what a
   reader absorbs. Measured: a comment line carries a median of **13 words** in both kinds, so the
   PO's line yardstick (class 10, method/attribute 3) converts to **130 / 40 / 40 words**.

`DOC DEVIATION` is the signed % against those ideals. `DEV WORDS` is a raw count with no ideal,
because zero IS the ideal there — measuring deviation from "one line" made the best case (no
developer note at all) read as -100%, i.e. worst. Rows sort by |deviation| so both failure modes —
bloated and missing — surface together.

A `VIS` column separates public from private: doxygen publishes only public members, so an
undocumented public method is an API gap while a private one is a maintenance note. Both are
reported; a bloated comment is bloat either way.

**No cutoff is enforced.** The ideals are a ruler — `MoonI80Peripheral`'s header may be right at
ten times the ideal, because it IS the driver's spec.

## Grew out of this plan: the host runs every driver

Not in the original scope, added on PO direction while implementing. The plan predicted a
permanent coverage limit — 184 `///` blocks in `src/light/drivers/` that the desktop AST could
never see, because the drivers were `#if defined(CONFIG_SOC_*)`-gated in `main.cpp`. The PO's
question ("shouldn't we instead run all existing drivers on the desktop?") turned out to be
right, and the premise wrong: those headers have zero direct ESP includes, already route
everything through `platform.h`, and already had desktop stubs. Only the *constants* said "not
my chip".

So the host now **emulates** the peripherals rather than declaring itself incapable:
`lcdLanes`/`parlioLanes` 16, `rmtTxChannels` 4, `hasLcdCam` true, and the platform seams back the
buses with heap memory instead of returning `false`/`nullptr`. `ParallelLedDriver` runs on macOS
against all three real backends, switchable live.

Recorded as a hard rule in [architecture.md § Platform abstraction](../../../architecture.md). Its
limit is deliberate: timing, wire protocol and pin state are NOT emulated, because faking them
would let a self-test report on hardware it never touched.

Coverage side effects, all from the same change: doc comments 454 → 629, RAM-costing arrays
362 → 390, heap allocation sites 63 → 80. Three ESP32 firmwares stayed byte-identical.

**Follow-up the PO raised, not yet planned:** a host LED-strip emulator that decodes the encoded
buffer back to RGB — `busLoopback` is already the "read back what the wire carried" seam, with a
fully specified `RmtLoopbackResult`. That would catch encode bugs `PreviewDriver` structurally
cannot, since it shows the *layer* buffer rather than the wire.

## Deliberately not in this plan

- **A `//` comment tool** — lizard's `LINES − NLOC` already covers 94% of them. The follow-up is
  to *surface* that number in the lizard report, which is a one-liner, not a tool.
- **Replacing lizard with an AST complexity rule.** Probed and viable:
  `cxxMethodDecl(hasName("tick"), ofClass(hasName("SolidEffect")))` dumps the method named
  **`tick`** where lizard reports `SolidEffect::static_cast<lengthType>`, and 25 branch nodes are
  countable in the dump — so the AST fixes the exact naming bug that leaves 35 of 162 baseline
  entries pinning nothing. Separate plan because it is a bigger change: `collect_kpi.py:31`
  hard-imports `check_lizard` at module level (deleting it breaks KPI collection at import), plus
  `repo_health.py:156-179`, `check_module.py:46`, `whitelizard.txt` and four docs. This plan
  proves the machinery on a new rule first; the complexity rule reuses it.
