#pragma once

#include "core/moonlive/MoonLive.h"
#include "platform/platform.h"

#include <cstdio>
#include <cstring>

namespace mm::moonlive {

/// Where a scripted module's script file lives. One fixed directory, the way `/.config` holds
/// persisted state: a module stores a NAME, not a path, so it cannot reach outside this folder and
/// the File Manager has one obvious place to look.
/// The USER's scripts, which the UI writes and the loader prefers. An edit made in the device's
/// own editor lands here, so a factory script of the same name is shadowed from that moment on:
/// pushing a new version of a shipped script to `kFactoryScriptDir` will NOT be seen once a user
/// has saved their own copy. (Two names differing only in case are one file on a case-insensitive
/// desktop and two on the device's LittleFS, which makes that shadowing easy to miss.)
inline constexpr const char* kScriptDir = "/moonlive";

/// Where the FACTORY scripts land: the ones the picker offers from the shipped catalog and the UI
/// downloads on first use. Separate from kScriptDir, and that split is the whole revert mechanism.
///
/// The script editor only ever saves to kScriptDir, so editing a factory script writes a second
/// file of the same name there rather than changing this one, and resolveScript below prefers it.
/// Un-editing is then deleting that copy, a LOCAL operation: with one directory an edit would
/// overwrite the only copy and getting the original back would mean downloading it again, needing
/// internet at exactly the moment a rig is already on site.
///
/// Dot-prefixed for the same reason `/.config` is: the File Manager hides it unless `hidden=1`, so
/// the factory copies do not clutter the tree, while staying plain readable text for anyone who
/// looks. A library you learn from has to be readable.
inline constexpr const char* kFactoryScriptDir = "/.moonlive";

/// A script's ROLE, in its file name. One language, five extensions: an effect is `.mle`, a
/// layout `.mll`, a modifier `.mlm`, a service `.mls`, a palette `.mlp`.
///
/// Stated by the author rather than derived from the script's contents. Deriving it is tempting
/// (the entry point a class defines already tells the engine which moment to call), but that
/// couples a UI filter to a language feature: the day a modifier wants a per-frame `tick()`, every
/// modifier would start appearing in effect pickers with no code changed anywhere. A name is
/// explicit, visible in any file listing without opening the file, and cannot drift.
///
/// The engine itself is role-BLIND and stays that way: it runs whichever moment the binding asks
/// for, so a class defining several is still legal. The extension decides which picker offers a
/// file, not what the engine will do with it.
inline constexpr const char* kEffectExt   = ".mle";
inline constexpr const char* kLayoutExt   = ".mll";
inline constexpr const char* kModifierExt = ".mlm";
inline constexpr const char* kServiceExt  = ".mls";
inline constexpr const char* kPaletteExt  = ".mlp";

/// What a NEW script starts out as, per role. A created file is a WORKING example rather than an
/// empty one: an empty file fails to parse the moment it is made, so the first thing a new script
/// would say is an error message. Each role gets the moment it is actually asked about (`tick` for
/// an effect, `placeLights` for a layout, `modifyLogical` for a modifier) and one control, so the
/// shape of a script is visible before a line is typed.
inline constexpr const char* kEffectTemplate =
    "class NewEffect {\n"
    "  byte bpm = 60;\n"
    "\n"
    "  void defineControls() {\n"
    "    addControl(\"bpm\", bpm, 1, 255);\n"
    "  }\n"
    "\n"
    "  void tick() {\n"
    "    fill(scale(beat(bpm, t), 256), 0, 100);\n"
    "  }\n"
    "}\n";

inline constexpr const char* kLayoutTemplate =
    "class NewLayout {\n"
    "  byte cols = 16;\n"
    "  byte rows = 16;\n"
    "\n"
    "  void defineControls() {\n"
    "    addControl(\"cols\", cols, 1, 64);\n"
    "    addControl(\"rows\", rows, 1, 64);\n"
    "  }\n"
    "\n"
    "  void placeLights() {\n"
    "    for (y = 0; y < rows; y = y + 1) {\n"
    "      for (x = 0; x < cols; x = x + 1) {\n"
    "        addLight(x, y, 0);\n"
    "      }\n"
    "    }\n"
    "  }\n"
    "}\n";

inline constexpr const char* kModifierTemplate =
    "class NewModifier {\n"
    "  void modifyLogical() {\n"
    "    setXYZ(width - 1 - xPos, yPos, zPos);\n"
    "  }\n"
    "}\n";

/// A SERVICE reads hardware and drives controls, so its template is a button: poll a pin on the
/// 50 Hz tick, and on a change write the control surface. It shows the two things that make a
/// service a service (`tick20ms` rather than `tick`, and `setControl` as the output) and the edge
/// state that stops it firing every tick, which is the thing a mapping row cannot express.
inline constexpr const char* kServiceTemplate =
    "class NewService {\n"
    "  int pin = 0;\n"
    // last starts at 1, the level an idle pull-up reads: starting at 0 made the first tick see a
    // change that had not happened and write the control before anyone touched the button.
    "  int last = 1;\n"
    "\n"
    "  void defineControls() {\n"
    "    addControl(\"pin\", pin, 0, 48);\n"
    "  }\n"
    "\n"
    "  void tick20ms() {\n"
    "    int now = gpioRead(pin);\n"
    "    if (now != last) {\n"
    "      last = now;\n"
    // INVERTED: the wiring is active-low (a switch to ground with a pull-up), so a pressed button
    // reads 0 and the switch it drives wants 1.
    "      setControl(\"switch1\", 1 - now);\n"
    "    }\n"
    "  }\n"
    "}\n";

/// What a `script` control tells the UI: where the files are, which of them to offer, and what a
/// new one starts as. Borrowed by the control descriptor (addFilePath), so these live here next to
/// the directory they name rather than being repeated in each binding.
inline constexpr const char* kPaletteTemplate =
    "class NewPalette {\n"
    "  byte bpm = 20;\n"
    "\n"
    "  void defineControls() {\n"
    "    addControl(\"bpm\", bpm, 1, 120); // how fast the colors move\n"
    "  }\n"
    "\n"
    "  void tick() {\n"
    "    for (int i = 0; i < 16; i = i + 1) {\n"
    "      setPalEntryHSV(i, scale(beat(bpm, t), 256) + i * 16, 255, 255);\n"
    "    }\n"
    "  }\n"
    "}\n";

inline constexpr const char* kEffectPick[3]   = {kScriptDir, kEffectExt,   kEffectTemplate};
inline constexpr const char* kLayoutPick[3]   = {kScriptDir, kLayoutExt,   kLayoutTemplate};
inline constexpr const char* kModifierPick[3] = {kScriptDir, kModifierExt, kModifierTemplate};
inline constexpr const char* kServicePick[3]  = {kScriptDir, kServiceExt,  kServiceTemplate};
inline constexpr const char* kPalettePick[3]  = {kScriptDir, kPaletteExt,  kPaletteTemplate};

/// Is `ext` one of the script extensions? One definition, beside the extensions themselves.
///
/// It lives here because two test files had each grown their own copy: when `.mls` was added to one
/// of them the other kept the old list, and since both were `inline` the linker picked whichever it
/// liked. The result was a sweep that could not see a whole role of script while every other check
/// agreed the file was there. A single definition is what makes that impossible.
inline bool isScriptExt(const char* ext) {
    if (!ext) return false;
    return std::strcmp(ext, kEffectExt) == 0 || std::strcmp(ext, kLayoutExt) == 0 ||
           std::strcmp(ext, kModifierExt) == 0 || std::strcmp(ext, kServiceExt) == 0 ||
           std::strcmp(ext, kPaletteExt) == 0;
}

/// The largest script the loader will read into RAM at once. Not a language limit — the buffer is
/// sized to the FILE and freed the moment the compile ends — but a bound so a stray large file
/// cannot ask a 320 KB device for an allocation it will not survive.
inline constexpr long kScriptFileMax = 16384;

/// Longest script name accepted, and the bound on the path buffer below. The bindings size their
/// `script` control buffer from this (`kMaxScriptName + 1`), so a name this loader would accept can
/// always be held: a shorter control would truncate silently, and truncation can strip the
/// extension that makes a name valid at all.
inline constexpr size_t kMaxScriptName = 40;

/// Read `<kScriptDir>/<name>` and compile it. The source lives in a right-sized heap buffer for the
/// duration of the compile and is freed before returning, so a module holds a filename (~32 B) and
/// the emitted code — never the script text. That is the whole point: the fixed per-module arrays
/// this replaces cost ~2 KB EACH, resident whether or not a script was loaded.
///
/// Returns true when the script compiled. On any failure `err` names it, in the words a user needs:
/// which file, and what was wrong with it.
/// FNV-1a over the script text. A caller that must know "did this change" keeps 4 bytes rather than
/// a second copy of the source, which is the whole reason the text is not resident any more.
inline uint32_t scriptHash(const char* s, size_t len) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) { h ^= static_cast<uint8_t>(s[i]); h *= 16777619u; }
    return h;
}

/// Where `name` actually lives: the user's copy if there is one, else the factory copy.
///
/// ONE resolver for both readers below. They used to build the path themselves, and the day a
/// second directory appeared that would have been two places to keep in step: a fork compiled from
/// kScriptDir while its hash came from the factory copy would look changed on every prepare sweep
/// and recompile forever.
///
/// Writes the resolved path into `out` and returns true when a file is there. False means neither
/// directory has it, and `out` then holds the USER path, so a caller reporting an error names the
/// place a user would put one.
inline bool resolveScript(const char* name, char* out, size_t outLen) {
    std::snprintf(out, outLen, "%s/%s", kScriptDir, name);
    if (platform::fsSize(out) >= 0) return true;
    char factory[96];
    std::snprintf(factory, sizeof(factory), "%s/%s", kFactoryScriptDir, name);
    if (platform::fsSize(factory) < 0) return false;   // neither: leave `out` as the user path
    std::snprintf(out, outLen, "%s", factory);
    return true;
}

/// LINEAGE: the hash of the shipped copy a user's edit was forked FROM, kept beside the fork.
///
/// The fork itself cannot carry it. A script is user-facing text shown in the device's own editor,
/// so a provenance line in the file would be visible, editable and lost on the first paste. A
/// sidecar is the standard answer (a `.orig` next to a patched config), and hidden here for the
/// same reason `/.moonlive` is: the File Manager does not show dot files unless asked.
///
/// It answers the one question the shadow marker cannot: has the SHIPPED copy moved since the fork
/// was made? Without it, an edit and a stale leftover look identical forever, which is what let 29
/// pre-`void tick()` copies sit on a bench P4 failing every compile with offsets into files nobody
/// had written that day.
inline void scriptLineagePath(const char* name, char* out, size_t outLen) {
    std::snprintf(out, outLen, "%s/.%s.from", kScriptDir, name);
}

/// Record that the user's copy of `name` was forked from text hashing to `hash`.
inline bool noteScriptLineage(const char* name, uint32_t hash) {
    if (!name || !name[0]) return false;
    char path[128];
    scriptLineagePath(name, path, sizeof(path));
    char text[16];
    const int n = std::snprintf(text, sizeof(text), "%u", static_cast<unsigned>(hash));
    return n > 0 && platform::fsWriteAtomic(path, text, static_cast<size_t>(n));
}

/// The hash a fork was made from, or false when no lineage was recorded (an older fork, or a script
/// the user wrote themselves). Absent lineage means "cannot say", never "unchanged".
inline bool scriptLineage(const char* name, uint32_t& out) {
    if (!name || !name[0]) return false;
    char path[128];
    scriptLineagePath(name, path, sizeof(path));
    char text[16] = {};
    const int got = platform::fsRead(path, text, sizeof(text));
    if (got <= 0) return false;
    uint32_t v = 0;
    for (int i = 0; i < got && text[i] >= '0' && text[i] <= '9'; i++)
        v = v * 10u + static_cast<uint32_t>(text[i] - '0');
    out = v;
    return true;
}

/// True when the user's copy of `name` is HIDING a shipped one: both directories hold it, so the
/// resolver above compiles the user's and every push to the factory copy is invisible. The binding
/// says so in its status, because from outside the two cases look identical, and a stale user copy
/// has twice been chased as a compiler bug: once as a control that never appeared, once as an
/// old-syntax copy failing with offsets that matched nothing in the file just written.
/// Record lineage for a just-written file, if it is a fork: a path in the user script directory
/// naming a script the firmware also ships. Anything else (a config file, a user's own script, a
/// write to the factory directory) is not a fork and is left alone.
///
/// Takes the WRITTEN path rather than a name, so the one caller is the write hook and no caller has
/// to work out whether a write was a fork.
inline void noteForkedFrom(const char* path) {
    if (!path) return;
    char prefix[64];
    const int plen = std::snprintf(prefix, sizeof(prefix), "%s/", kScriptDir);
    if (plen <= 0 || std::strncmp(path, prefix, static_cast<size_t>(plen)) != 0) return;
    const char* name = path + plen;
    if (!name[0] || std::strchr(name, '/') || name[0] == '.') return;   // nested, or our own sidecar

    // A REVERT arrives here too: applyFileChanged fires on a delete as well as a write, and the
    // fork is already gone by then. Drop the lineage with it, or a later fork of the same name
    // would be compared against a hash from a fork that no longer exists.
    char user[96];
    std::snprintf(user, sizeof(user), "%s/%s", kScriptDir, name);
    if (platform::fsSize(user) < 0) {
        char side[128];
        scriptLineagePath(name, side, sizeof(side));
        platform::fsRemove(side);
        return;
    }

    // Only when the fork is CREATED. Re-stamping on every save would mark the fork up to date with
    // whatever the library holds at that moment, which silently erases the very thing the record
    // exists to show: that the shipped copy moved while the user was not looking. Lineage is the
    // point the fork BRANCHED from, and a branch point does not move.
    uint32_t already = 0;
    if (scriptLineage(name, already)) return;

    char factory[96];
    std::snprintf(factory, sizeof(factory), "%s/%s", kFactoryScriptDir, name);
    const long size = platform::fsSize(factory);
    if (size <= 0 || size > kScriptFileMax) return;                     // nothing shipped: not a fork
    char* text = static_cast<char*>(platform::alloc(static_cast<size_t>(size) + 1));
    if (!text) return;
    const int got = platform::fsRead(factory, text, static_cast<size_t>(size) + 1);
    if (got > 0) noteScriptLineage(name, scriptHash(text, static_cast<size_t>(got)));
    platform::free(text);
}

/// True when the SHIPPED copy has changed since the user forked it: both files exist, a lineage
/// was recorded, and the factory copy no longer hashes to what the fork was made from.
///
/// False when there is no lineage. An older fork, or a script the user wrote, cannot be compared,
/// and claiming an update on a guess would send someone to discard work for nothing.
inline bool scriptFactoryMovedOn(const char* name) {
    uint32_t from = 0;
    if (!scriptLineage(name, from)) return false;
    char path[96];
    std::snprintf(path, sizeof(path), "%s/%s", kFactoryScriptDir, name);
    const long size = platform::fsSize(path);
    if (size <= 0 || size > kScriptFileMax) return false;
    char* text = static_cast<char*>(platform::alloc(static_cast<size_t>(size) + 1));
    if (!text) return false;
    const int got = platform::fsRead(path, text, static_cast<size_t>(size) + 1);
    const bool moved = got > 0 && scriptHash(text, static_cast<size_t>(got)) != from;
    platform::free(text);
    return moved;
}

inline bool scriptShadowsFactory(const char* name) {
    char path[96];
    std::snprintf(path, sizeof(path), "%s/%s", kScriptDir, name);
    if (platform::fsSize(path) < 0) return false;
    std::snprintf(path, sizeof(path), "%s/%s", kFactoryScriptDir, name);
    return platform::fsSize(path) >= 0;
}

/// The hash of `name`'s CURRENT text, without compiling it.
///
/// Answers "has the file changed since I compiled it" for the cost of ONE read, which is what a
/// binding asks on every prepare sweep. It costs the same whole-file read compileScriptFile makes
/// and skips everything after: the parse, the codegen, and the exec-block allocation.
///
/// False when the file is missing, unreadable or outside the accepted bounds, which the caller
/// treats as "not the thing I compiled" and lets compileScriptFile report properly. Reporting the
/// diagnostic here too would put the same message in two places.
inline bool scriptFileHash(const char* name, uint32_t& out) {
    if (!name || !name[0]) return false;
    char path[96];
    if (!resolveScript(name, path, sizeof(path))) return false;
    const long size = platform::fsSize(path);
    if (size <= 0 || size > kScriptFileMax) return false;

    // ONE read of the WHOLE file, not a chunked walk. fsReadAt opens and closes the file on every
    // call, so hashing a 2 KB script through a small stack window cost 16 open/close cycles per
    // module per prepare sweep. On a P4 that boot-looped with `Cache error`: LittleFS sits behind
    // the flash cache and the sweep runs three scripted modules at once.
    //
    // Hashing only a WINDOW was the other tempting fix and is worse: four shipped scripts are over
    // 1 KB, so an edit past the window would go undetected and the module would keep running the
    // previous program. A change-detector that misses changes is not one.
    //
    // The heap allocation is the same one compileScriptFile makes, on the same cold path, and it is
    // freed before returning. It buys the whole file with one open, and this runs only when a
    // prepare sweep asks, not per frame.
    char* text = static_cast<char*>(platform::alloc(static_cast<size_t>(size) + 1));
    if (!text) return false;                          // no memory is "cannot answer", not "unchanged"
    const int got = platform::fsRead(path, text, static_cast<size_t>(size) + 1);
    const bool ok = got > 0;
    if (ok) out = scriptHash(text, static_cast<size_t>(got));
    platform::free(text);
    return ok;
}

/// As compileScriptFile, and additionally reports the source's hash so a caller can tell a changed
/// script from an unchanged one without holding the text.
inline bool compileScriptFile(MoonLive& engine, const char* name,
                              const BuiltinTable& builtins, const SysVarTable& sysvars,
                              const char*& err, uint32_t* hashOut = nullptr) {
    // FIRST, before any validation can return: drop whatever is already compiled. Every check
    // below leaves through `return false`, and only engine.compile() releases the previous
    // program, so without this a rejected script (renamed, deleted, emptied) leaves the OLD one
    // executing while the module reports an error. The card says "script not found" and the
    // fixture keeps rendering the script that is gone.
    //
    // freeCode, not free: the control ARENA must survive, or a scripted control loses the live
    // value the user set whenever a compile fails.
    engine.freeCode();

    // The script directory must exist before anything can be SAVED into it, and on a fresh device
    // nothing has created it yet — the write endpoint does not make parent directories, so a first
    // save would fail with nowhere obvious to look. Creating it here (mkdir -p, a no-op when it is
    // already there) means naming a script is enough to make the folder appear.
    platform::fsMkdir(kScriptDir);

    if (!name || !name[0]) { err = "no script — set the script name"; return false; }

    // A BASENAME only. The fixed directory is the point — a module names a script, it does not
    // address the filesystem — so a separator or a `..` would let a control value reach outside
    // kScriptDir (`../.config/NetworkModule.json` reads the device's saved credentials). Rejected
    // rather than sanitised: a name that needs rewriting to be safe is a name a user mistyped.
    for (const char* c = name; *c; c++)
        if (*c == '/' || *c == '\\') { err = "script name is a file in the script folder, not a path"; return false; }
    if (std::strcmp(name, "..") == 0 || std::strncmp(name, "../", 3) == 0) {
        err = "script name is a file in the script folder, not a path"; return false;
    }
    // One of the three script extensions, so a stray name cannot pull in an unrelated file that
    // happens to sit alongside. ANY of them: the loader is role-blind, exactly as the engine is,
    // and which picker offered the file is the binding's business. The upper bound also lets the
    // compiler see that the snprintf below cannot truncate.
    const size_t len = std::strlen(name);
    const char* tail = len >= 4 ? name + len - 4 : "";
    if (len < 5 || len > kMaxScriptName || !isScriptExt(tail)) {
        err = "script name must end in .mle, .mll, .mlm, .mls or .mlp"; return false;
    }

    // The user's copy wins over the factory one of the same name: that is what makes editing a
    // factory script a fork rather than a change to it.
    char path[96];
    resolveScript(name, path, sizeof(path));

    const long size = platform::fsSize(path);
    if (size < 0)               { err = "script not found"; return false; }
    if (size == 0)              { err = "script is empty";  return false; }
    if (size > kScriptFileMax)  { err = "script too large"; return false; }

    // +1 for the NUL the lexer reads as End. fsRead null-terminates on success, but the buffer has
    // to have room for it.
    char* text = static_cast<char*>(platform::alloc(static_cast<size_t>(size) + 1));
    if (!text) { err = "no memory for the script"; return false; }

    const int read = platform::fsRead(path, text, static_cast<size_t>(size) + 1);
    if (read <= 0) { platform::free(text); err = "script could not be read"; return false; }

    if (hashOut) *hashOut = scriptHash(text, static_cast<size_t>(read));
    const bool ok = engine.compile(text, builtins, sysvars);
    if (!ok) err = engine.error();
    // Freed on BOTH paths, before returning: the text has done its job either way, and a failed
    // compile is exactly when a device can least afford to leak.
    platform::free(text);
    return ok;
}

}  // namespace mm::moonlive
