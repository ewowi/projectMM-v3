// The live-patch path must not rewrite text that has not changed.
//
// Why this matters: assigning `textContent` REPLACES the element's text node even when the
// string is identical, and replacing a text node collapses any selection inside it. The UI
// patches every visible module once a second, so an unconditional write made a value
// impossible to select and copy: the highlight died under the cursor within a second. The
// worst case was a value that never changes at all (a status line reading the same string
// forever), because there the rewrite was pure loss.
//
// `setText(el, text)` writes only on a real change, so this pins that every text write inside
// the once-a-second patch path goes through it. A direct `textContent =` in a build-once path
// is fine (the node is new, nothing can be selected in it yet) — this checks the live path
// only, which is why it is bounded by the two function names rather than the whole file.
//
// Run: `node --test test/js`.

import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";

const src = readFileSync(new URL("../../src/ui/app.js", import.meta.url), "utf8");

/// The body of a top-level `function <name>(`, by brace matching.
function functionBody(name) {
    const start = src.indexOf(`function ${name}(`);
    assert.notEqual(start, -1, `${name} not found in app.js`);
    const open = src.indexOf("{", start);
    let depth = 0;
    for (let i = open; i < src.length; i++) {
        if (src[i] === "{") depth++;
        else if (src[i] === "}" && --depth === 0) return src.slice(open, i + 1);
    }
    assert.fail(`unbalanced braces in ${name}`);
}

// Both halves of the per-second patch: the module-level stats/status line and the per-control
// values. These run for every visible module on every state patch.
// EVERY function on the once-a-second patch path, not just the two obvious ones. The bug came
// back twice after being "fixed" because the audit was per-function: the status bar and the tab
// dot are patched on the same tick and were missed. Adding a function to updateValues without
// adding it here is the gap this list closes.
for (const fn of ["updateValues", "updateModuleControls", "updateStatusBar", "applyTabDot",
                  "setStatusText"]) {
    test(`${fn} writes text only through setText, so a selection survives the patch`, () => {
        const body = functionBody(fn);
        const offenders = body
            .split("\n")
            .map((line, i) => [i + 1, line])
            // A write is `.textContent =` or `.innerHTML =` (but not the `!==` comparison
            // setText itself makes, and not a line that only reads the property).
            .filter(([, line]) => /\.(textContent|innerHTML)\s*=[^=]/.test(line))
            // Two writes are legitimate and must not be flagged:
            //   - a node CREATED in this pass (a status row inserted the first time a module
            //     reports status, the <option>s of a rebuilt list). Nothing can be selected in
            //     a node that did not exist a moment ago.
            //   - a write already inside an explicit "did it change?" branch, which is the same
            //     discipline setText applies, just structural (a select whose whole option list
            //     is replaced only when the options actually differ).
            .filter(([n, line]) => {
                const target = line.trim().split(/\.(textContent|innerHTML)/)[0]
                                   .replace(/^.*[\s(]/, "").trim();
                const created =
                    new RegExp(`${target}\\s*=\\s*document\\.createElement`).test(body);
                // Look back a few lines for a guard that gates THIS element's write. It has to
                // name the same element, or any unrelated `if (a !== b)` nearby would exempt it:
                // that looseness let a bare statsEl.textContent write through undetected.
                const before = body.split("\n").slice(Math.max(0, n - 6), n - 1).join(" ");
                const guarded = new RegExp(`if\\s*\\([^)]*\\b${target}\\b[^)]*(!==|!=)`).test(before)
                                || /if\s*\([^)]*(\.some\(|length\s*!==)/.test(before);
                return !created && !guarded;
            });

        assert.deepEqual(
            offenders.map(([n, l]) => `${n}: ${l.trim()}`),
            [],
            `write text through setText(el, value) instead: a bare textContent assignment ` +
            `replaces the text node every second and kills the user's selection`);
    });
}

// setText itself must actually compare before writing; without the guard every call site above
// is silently back to an unconditional write.
test("setText writes only when the text actually differs", () => {
    const body = functionBody("setText");
    assert.match(body, /textContent\s*!==/,
                 "setText must compare the current text before assigning");
});

// Text is not the only thing that kills a selection: REMOVING a node inside the selected range
// collapses it too. applyTabDot used to remove and recreate the tab's status dot on every patch,
// which made text in that card impossible to copy even though no text write was involved. Any
// remove() on the patch path has to be gated on the thing actually having changed.
for (const fn of ["updateValues", "updateModuleControls", "updateStatusBar", "applyTabDot"]) {
    test(`${fn} removes nodes only when something actually changed`, () => {
        const body = functionBody(fn);
        const lines = body.split("\n");
        const offenders = lines
            .map((line, i) => [i + 1, line])
            .filter(([, line]) => /\.remove\(\)/.test(line))
            .filter(([n]) => {
                // An early return on "nothing changed" above the remove is the guard.
                const before = lines.slice(0, n).join(" ");
                return !/if\s*\([^)]*(===|!==)[^)]*\)\s*return/.test(before);
            });
        assert.deepEqual(
            offenders.map(([n, l]) => `${n}: ${l.trim()}`),
            [],
            `gate this remove() on a real change: removing a node inside the user's selection ` +
            `collapses it, so an unconditional remove every second blocks copying`);
    });
}

// The lists above are only as good as their coverage: a new helper called from updateValues that
// nobody adds here is exactly how this bug came back twice. Pin that every function updateValues
// calls into (our own, not DOM builtins) is one this file checks.
test("every function on the patch path is covered by this file", () => {
    const checked = ["updateValues", "updateModuleControls", "updateStatusBar", "applyTabDot",
                     "updateTabDot", "setText", "setUrlDisplay", "setStatusText"];
    const keywords = ["if", "for", "while", "switch", "catch", "return", "typeof"];
    const dom = ["querySelector", "querySelectorAll", "queryByName", "createElement", "appendChild",
                 "insertBefore", "toggle", "setAttribute", "getAttribute", "remove", "closest",
                 "matches", "getSelection", "String", "Number", "Array", "Math", "JSON"];
    const body = functionBody("updateValues");
    const called = [...new Set(Array.from(body.matchAll(/\b([a-zA-Z_][\w]*)\s*\(/g), m => m[1]))]
        .filter(n => !dom.includes(n) && !keywords.includes(n))
        // Formatters return strings and touch no DOM, so they cannot break a selection.
        .filter(n => !/^(fmt|format|css|now|allModules|previewTargetFps)/.test(n));
    const missing = called.filter(n => !checked.includes(n));
    assert.deepEqual(missing, [],
        `these run on every state patch but are not audited here: add them to the lists above ` +
        `(or to the formatter/DOM exemptions if they cannot touch the DOM)`);
});
