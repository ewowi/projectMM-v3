# Plan — Docs system overhaul: Phase 0 → Docs v2 (shipped)

**Consolidated record.** This one plan replaces the nine individual docs-overhaul plans of
2026-06-30 → 2026-07-02 (Phase 0, per-type consolidation, Phase 1+2, Phase 3, Phase 4a, two Phase 4b
variants, and Docs v2). Every phase below **shipped** except the Doxide pilot, which was attempted and
abandoned — its goal (developer drill-down into the source API) was delivered instead by moxygen in
Docs v2. (The parent design study, `docs/backlog/docs-system-overhaul.md`, was itself fully shipped
and pruned per *Mandatory subtraction* — this plan is the surviving record.)

## The arc

Docs started as 259K words of raw `.md` read on github.com — no landing page, nav, or search, and
per-module `.md` files that hand-duplicated facts the `.h` already stated. The overhaul moved to a
**rendered site** with **two documentation surfaces per module**: a hand-written summary/catalog page
(end-user) and a technical page **generated from the `.h`** (developer). Facts now live once — in the
`.h` `///` (→ generated page) or the summary row — never a third per-module `.md`.

The phases shipped additively (each a visible, revertible win), per *Refactor for simplicity* — never
a big-bang.

## What shipped, by phase

### Phase 0 — stand up the site (MkDocs Material)
`mkdocs.yml` (Material, instant search, top-down user→developer nav), `docs/index.md` landing page
(absorbed the retired `docs/landing/index.html`), `moondeck/docs/build_docs.py` (uv wrapper around
`mkdocs build`), CI `deploy-pages` builds the site at Pages root `/`, installer stays at `/install/`.
**Landed alongside:** de-overloaded `docs/` — the standalone web installer moved to a top-level
`web-installer/` (it's an app, not docs; deployed URL unchanged), `history/`+`backlog/` kept in `docs/`
but excluded from the published nav. ~104 `docs/install/`→`web-installer/` references swept.

### Per-type doc consolidation (Stage 2 of the MoonLight migration)
~26 per-module effect/modifier/layout `.md` files → three compact-row pages (`effects.md`,
`modifiers.md`, `layouts.md`) with library *sections* inside (MoonLight / WLED / FastLED /
projectMM-native), one table row per module. `check_specs.py` moved from file-scoped to **page-scoped**
control-name validation. Drivers stayed per-file at the time (later folded into `drivers.md` +
per-driver moxygen pages by Docs v2). Library stays a *tag* + a *doc split*, never a folder axis (the
[folder-structure decision](../../../adr/0015-library-is-a-tag-not-a-folder.md)).

### Phase 1+2 — nav fold + generated tests in the build
Phase 1 (audience-split nav) was mostly delivered by Phase 0. Phase 2: the test-inventory pages
(`tests/unit-tests.md` + `scenario-tests.md`) are **generated at build time** from the test files (via
the `mkdocs_hooks.py` `on_files` hook calling `render_unit_tests`/`render_scenarios`) and **gitignored**
(`.gitignore: /docs/tests/*.md`) — ~25K committed words removed, can't drift. Each catalog card keeps a
compact one-line `[Tests]` **link** into its inventory section; an attempt to inline the full case list
per card bloated the cards and was reverted (**tests are a link, not a dump** — the deliberate 2b
outcome).

### Phase 3 — drift validation (not snippet de-dup)
The snippet-include premise was wrong: `.h` and `.md` hold the *same fact in two forms* (code vs
prose), which `--8<--` can't bridge. The real duplication was narrower — control **ranges** (~50) and
author **URLs** (~51) restated in both places. PO chose **validate, not generate**: `check_specs.py`
gained `_check_range_drift` + `_check_author_url_drift` (block-scoped on catalog pages, tolerant of
human range spellings), pinned by `test/python/test_check_specs_drift.py`. Control *names* and
architectural facts were confirmed NOT duplication (audience-aware) and left alone.

### Phase 4a — source snippets
`pymdownx.snippets` (`--8<--`) embeds real source into a doc where the source *is* the spec: the
`ImprovFrameType` enum + magic/payload constants (`ImprovFrame.h`) and the Preview wire-format
(`PreviewDriver.h`). Editing the `.h` constant changes the rendered doc — single-source, no drift.

### Phase 4b — Doxide pilot: ATTEMPTED, ABANDONED
Doxide was built from source and run on real headers. It **choked** on projectMM's C++20 (Tree-sitter
parse errors on `auto**`/pointer-to-member on the first core file), renders **only** Doxygen-commented
entities (our `//` comments produced empty pages → would need converting all 139 headers), and has
near-zero adoption. High-cost, high-risk, premature — **dropped**. Its goal (in-site full source API
docs) was the same one Docs v2 then delivered by a lighter route.

### Docs v2 — two-surface module docs (the source-generated goal, delivered)
The realisation of "the `.h` is the doc basis" — **not** via Doxide but via **moxygen** (Doxygen XML →
Handlebars templates → Markdown, in [`gen_api.py`](../../../moondeck/docs/gen_api.py)). Every module
gets a generated technical page at `docs/moonmodules/{core,light}/moxygen/<Module>.md` (gitignored,
built fresh) from its `.h` `///` comments; each catalog summary row links to it. Shipped in five stages:
1. **Machinery** — domain-nested moxygen output, module discovery from `src/{core,light}`.
2. **Template shape** — public-only reference (Handlebars denylist on private sections), `.md` on disk.
3. **Working system** — all pages generated + the summary pages built, *alongside* the old `.md` (a
   committable baseline, nothing deleted yet).
4. **Optimize** — swept `///` comments module-by-module so each generated page reads as excellent
   developer docs; added the summary-page control-name drift guard.
5. **Switchover** — deleted the ~30 old per-module `.md` (absorbed into `///` + summary rows), removed
   the temporary migration cross-check banner, reconciled architecture.md ↔ coding-standards.md on the
   two-surface model.

## Follow-on cleanups (post-Docs-v2, same arc)
- The per-module archive `.md` (the retired detail pages parked in `<domain>/archive/` during the
  migration) were validated against their generated pages and **deleted**; residual present-tense
  content that outlived its `.h` was migrated into `///`, forward-looking content into `docs/backlog/`.
- **`ui.md`** (the UI *system* spec, no `.h`) was promoted to a live page
  `docs/moonmodules/core/ui.md`, de-duplicated against architecture.md § Web UI + HttpServerModule.
- **Single-file catalog folders collapsed** (`light/effects/effects.md` → `light/effects.md`, etc.) —
  the flat layout the folder-structure decision prescribes as groundwork for future `effects_<library>.md`
  splits.
- **`check_specs.py` docPath guard** — validates every `main.cpp` `registerType` docPath resolves to a
  real page + `#anchor`, so a docs rename can't silently 404 the in-UI help links (the drift that a
  CodeRabbit review caught).

## Net result
- `docs/moonmodules/` holds only summary/catalog pages + the gitignored `moxygen/` generated pages;
  the ~30+ standalone per-module `.md` are gone (net doc-file subtraction).
- Every fact lives once — in the `.h` `///` or a summary row.
- The site renders at `moonmodules.org/projectMM/`; the installer at `/install/`.
- Drift is guarded at commit by `check_specs.py` (control names, ranges, URLs, docPaths).

## What's genuinely out (not deferred work, decided-against)
- **Doxide** — abandoned (above); moxygen delivered the goal.
- **Per-library page splits** (`effects_wled.md`) — future growth, a lift-not-rewrite when a library
  section outgrows its page; the flat filenames + sections are already in place for it.
- **assets/ and test/ type-splits** — the [folder-structure decision](../../../adr/0015-library-is-a-tag-not-a-folder.md)'s
  remaining "mirror src's domain/type shape" work; independent of the doc-content overhaul.
