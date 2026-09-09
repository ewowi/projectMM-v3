// Two modules whose names differ only in case must not share a card.
//
// The firmware compares module names with strcmp, so `lines` (a MoonLive effect) and `Lines` (a
// LinesEffect) are DIFFERENT modules: both are created, persisted and ticked. CSS disagrees.
// Attribute selectors match values case-INSENSITIVELY in an HTML document, so
// `.card[data-module="lines"]` also matches `data-module="Lines"` and querySelector returns
// whichever is first in the DOM. On the bench (2026-09-08) a Layer holding both rendered NO effect
// cards at all, while /api/modules/Effects returned both children correctly.
//
// Selectors Level 4 has a flag for exactly this (`[data-module="x" s]`) and CHROME DOES NOT SUPPORT
// IT: it throws SyntaxError, which takes out every card on the page. That was tried and reverted.
// The lookup is therefore narrowed in JS, by queryByName().
//
// Pinned by reading the source, the same way ui-render-guard does, because app.js is a browser
// script and a behavioural test would need a DOM.
//
// Run: `node --test "test/js/**/*.test.mjs"`.

import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const ROOT = join(dirname(fileURLToPath(import.meta.url)), "..", "..");
const app = readFileSync(join(ROOT, "src", "ui", "app.js"), "utf8");

// The four attributes that carry a MODULE NAME. data-key holds a control name and is not affected:
// controls live inside one module's card, so a case-clash there cannot cross modules.
const NAME_ATTRS = ["data-module", "data-mid", "data-tab-mid", "data-status-mid"];

test("no lookup keyed on a module name goes through a bare querySelector", () => {
    const bare = [];
    for (const attr of NAME_ATTRS) {
        const re = new RegExp(`document\\.querySelector\\(\\s*\`[^\`]*\\[${attr}="\\$\\{`, "g");
        for (const m of app.matchAll(re)) bare.push(`${attr} @${m.index}`);
    }
    assert.deepEqual(bare, [],
        `these would resolve to a module whose name differs only in case: ${bare.join(", ")}`);
});

test("the case-sensitivity flag is never used: Chrome throws SyntaxError on it", () => {
    // `[data-module="x" s]` is valid Selectors Level 4 and unsupported in Chrome. Using it does not
    // degrade, it throws, and every card on the page disappears.
    // Interpolated selectors only: a literal in a comment or a doc string is prose, not a lookup.
    const flagged = app.match(/\[data-[a-z-]+="\$\{[^}]*\}"\s+s\]/g) || [];
    assert.deepEqual(flagged, [], `the s flag is unsupported in Chrome: ${flagged.join(", ")}`);
});

test("queryByName compares the attribute exactly, and the app routes lookups through it", () => {
    const i = app.indexOf("function queryByName");
    assert.ok(i > 0, "queryByName must exist as the one home for name-keyed lookups");
    const body = app.slice(i, app.indexOf("\n}\n", i));
    assert.ok(body.includes("getAttribute"), "must read the attribute back to compare it exactly");
    assert.ok(/===/.test(body), "must compare with === so case is significant");
    // Vacuity guard: a regex that stopped matching would leave the first test green.
    const uses = app.match(/queryByName\(/g) || [];
    assert.ok(uses.length >= 25,
        `expected the app to route its name lookups through queryByName, found ${uses.length}`);
});
