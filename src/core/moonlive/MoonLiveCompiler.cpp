#include "core/moonlive/MoonLiveCompiler.h"
#include "core/moonlive/moonlive_emit.h"
#include "core/moonlive/MoonLiveIr.h"

#include <cstring>   // std::strncmp (keyword matching)

namespace mm::moonlive {

namespace {

// --- Lexer ---------------------------------------------------------------------------
// A `//` line comment is whitespace. `Assign` is `=` (a member declaration's initializer).
enum class Tok { Ident, Number, String, Assign, LParen, RParen, LBrace, RBrace, Comma, Semicolon,
                 Plus, Minus, Star, Slash, Percent, Less, LessEq, Greater, GreaterEq, EqEq, NotEq,
                 LBracket, RBracket, End, Error };

struct Lexer {
    const char* p;
    Tok kind = Tok::Error;
    int64_t number = 0;
    // Whether the literal just lexed carried a decimal point, i.e. it is a Q16.16 value.
    bool numberIsFixed = false;
    const char* identBeg = nullptr;
    size_t identLen = 0;
    const char* tokBeg = nullptr;
    const char* srcBeg;
    const char* err = "";

    explicit Lexer(const char* s) : p(s), srcBeg(s) { advance(); }

    static bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
    static bool isDigit(char c) { return c >= '0' && c <= '9'; }
    static bool isIdentStart(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
    static bool isIdentCont(char c) { return isIdentStart(c) || isDigit(c); }
    uint16_t col() const { return static_cast<uint16_t>((tokBeg - srcBeg) + 1); }

    // Read a number into v, and say whether it carried a decimal point.
    //
    // A decimal point makes it a FIXED literal: `1.5` reads as the Q16.16 word 98304, so a script
    // writes the number it means rather than the scaled integer. The fraction is accumulated as a
    // numerator over a power of ten and scaled once, which keeps it exact for the digits a script
    // would write.
    //
    // Overflow FAILS rather than truncating: the old cap stopped consuming digits at 1000000 and
    // left the value silently wrong, so `99999999` became a number nobody wrote.
    bool readNumber(int64_t& v, bool& isFixed, bool& overflowed) {
        if (!isDigit(*p)) return false;
        v = 0; isFixed = false; overflowed = false;
        // INT64 THROUGHOUT, and every check made BEFORE the multiply that could overflow.
        //
        // `long` is 32 bits on both ESP32 targets, and a script COMPILES ON THE DEVICE: the guard
        // `v > INT32_MAX` after `v = v*10 + d` could never fire there, because the multiply had
        // already wrapped (signed overflow, UB). `0.99999` was worse — num*65536 needs 33 bits.
        // Both produced a number nobody wrote, on hardware, while every host test stayed green.
        while (isDigit(*p)) {
            if (v > (INT64_MAX - 9) / 10) { overflowed = true; return true; }
            v = v * 10 + (*p - '0');
            p++;
            // The MAGNITUDE, not the value: a leading minus is a separate token, so 2147483648
            // is legal here and becomes INT32_MIN. The signed range is checked where the sign is
            // known — parsePrimary for an expression, parseDecl for an initializer.
            if (v > -static_cast<int64_t>(INT32_MIN)) { overflowed = true; return true; }
        }
        if (*p == '.' && isDigit(p[1])) {
            isFixed = true;
            p++;
            int64_t num = 0, den = 1;
            while (isDigit(*p)) {
                // Bounded so den * 65536 and num * 65536 both stay well inside int64; digits past
                // that precision are consumed and dropped rather than shifting the value.
                if (den <= 100000000LL) { num = num * 10 + (*p - '0'); den *= 10; }
                p++;
            }
            // The MAGNITUDE: 32768.0 is legal with a leading minus (the most negative Q16.16
            // value) and refused without one, which is a judgement only the sign-aware caller can
            // make. Same rule the integer path follows for 2147483648.
            if (v > 32768) { overflowed = true; return true; }
            // The integer part scales by 65536; the fraction is num/den of that, rounded.
            v = (v << 16) + (num * 65536 + den / 2) / den;
        }
        return true;
    }

    void advance() {
        for (;;) {
            while (isSpace(*p)) p++;
            // A line comment is whitespace, with no exception.
            if (p[0] == '/' && p[1] == '/') {
                p += 2;
                // Every line comment is whitespace. A comment that CHANGED BEHAVIOR lived here:
                // `// @control 1..120` declared a control's range, which is not C and does not
                // resemble the compiled module a script stands in for. `defineControls()` calling
                // `addControl("bpm", bpm, 1, 120)` replaced it, so the token, its capture and the
                // `lineStart` this needed are all gone.
                while (*p && *p != '\n') p++;
                continue;
            }
            break;
        }
        tokBeg = p;
        char c = *p;
        if (c == 0) { kind = Tok::End; return; }
        // Two-character operators first: '<' is a prefix of '<=' and '=' of '==', so testing a
        // short form ahead of the long one would lex `a == b` as two assignments. Maximal munch.
        if (c == '<' && p[1] == '=') { p += 2; kind = Tok::LessEq;    return; }
        if (c == '>' && p[1] == '=') { p += 2; kind = Tok::GreaterEq; return; }
        if (c == '=' && p[1] == '=') { p += 2; kind = Tok::EqEq;      return; }
        if (c == '!' && p[1] == '=') { p += 2; kind = Tok::NotEq;     return; }
        if (c == '=') { p++; kind = Tok::Assign; return; }
        if (c == '(') { p++; kind = Tok::LParen; return; }
        if (c == ')') { p++; kind = Tok::RParen; return; }
        if (c == ',') { p++; kind = Tok::Comma; return; }
        if (c == ';') { p++; kind = Tok::Semicolon; return; }
        if (c == '+') { p++; kind = Tok::Plus;    return; }
        if (c == '-') { p++; kind = Tok::Minus;   return; }
        if (c == '*') { p++; kind = Tok::Star;    return; }
        if (c == '{') { p++; kind = Tok::LBrace;  return; }
        if (c == '}') { p++; kind = Tok::RBrace;  return; }
        if (c == '[') { p++; kind = Tok::LBracket; return; }
        if (c == ']') { p++; kind = Tok::RBracket; return; }
        if (c == '<') { p++; kind = Tok::Less;    return; }
        if (c == '>') { p++; kind = Tok::Greater; return; }
        // '/' only reaches here when it is NOT the `//` a comment starts with (handled above).
        if (c == '/') { p++; kind = Tok::Slash;   return; }
        if (c == '%') { p++; kind = Tok::Percent; return; }
        // A quoted string: a control's UI label. The span goes in identBeg/identLen, the same
        // fields an identifier uses, because both are a run of source bytes the parser reads
        // without copying. No escapes: a control name with a quote or a newline in it is not a
        // thing anyone needs, and the absence is one less rule to document.
        if (c == '"') {
            p++;
            identBeg = p;
            while (*p && *p != '"' && *p != '\n') p++;
            if (*p != '"') { kind = Tok::Error; err = "unterminated string"; return; }
            identLen = static_cast<size_t>(p - identBeg);
            p++;                                    // the closing quote
            kind = Tok::String; return;
        }
        if (isDigit(c)) {
            int64_t v = 0; bool fx = false, over = false;
            readNumber(v, fx, over);
            if (over) { err = "number out of range"; kind = Tok::Error; return; }
            number = v; numberIsFixed = fx; kind = Tok::Number; return;
        }
        if (isIdentStart(c)) {
            identBeg = p;
            while (isIdentCont(*p)) p++;
            identLen = static_cast<size_t>(p - identBeg);
            kind = Tok::Ident; return;
        }
        kind = Tok::Error; err = "unexpected character";
    }
};

// --- Parser → IR ---------------------------------------------------------------------
// A recursive-descent parser that evaluates each expression into a virtual register and emits
// IR. It knows the GRAMMAR and resolves call names against the injected table; it owns no
// function names. Buffer writers (Kind::Inline) and helpers (Kind::Call) are dispatched
// generically.
struct Parser {
    Lexer&             lex;
    const BuiltinTable& table;
    const SysVarTable&  sysvars;
    IrProgram&         ir;
    char*              classNameOut = nullptr;   // the caller's buffer; the parser fills it
    // Each function the class defined, with the IR index its body starts at. An IR index, not a byte
    // offset: the parser runs before lowering, so the byte an entry lands on is not known yet. The
    // emitter converts one to the other, which is the same seam a linker crosses.
    /// `params` is how many arguments the function declares. They live in the LOWEST frame
    /// slots of its frame (0..params-1), which is what lets a caller stage arguments into them
    /// with the same Spill the builtin path already uses: no backend calling convention changes,
    /// and in particular Xtensa's windowed call8 sequence is untouched.
    struct FnMark { const char* name; uint8_t nameLen; uint16_t irStart; RetType ret; uint8_t params; };
    FnMark             fns[kMaxEntryPoints] = {};
    uint8_t            fnCount = 0;
    /// What the function currently being parsed declared it returns, so a `return` can be
    /// checked against it at the point it is written rather than after the fact.
    RetType            curRet = RetType::Void;
    VReg               nextTemp = kFirstTemp;   // high-water mark — also IrProgram.vregsUsed
    VReg               freeStack[kMaxVRegs] = {};   // recycled temps (LIFO), so a dead vreg is reused
    uint8_t            freeCount = 0;
    // Script-local variables — a `for` loop's counter and its limit. Distinct from a declared
    // control (a control is a UI value the script READS; a local is one the script WRITES) and from
    // a temp (a temp is write-once and recycled).
    //
    // A local lives in a FRAME SLOT, not a register. Holding it in a register for the whole of its
    // scope is what made a nested loop unfittable on the smallest target: Xtensa's windowed ABI
    // leaves ten registers, five carry the host arguments, and the reload temps take the rest — so a
    // loop counter that never yields its register left nothing to compute with and every looped
    // script was refused. In the frame, the number of live variables is bounded by memory rather
    // than by the register file, which is the whole point of the stack machine, and it is also what
    // makes function arguments and recursion fall out later rather than needing new machinery.
    // `isFixed` is the local's half of the scaling wall the members already enforce: a Q16.16
    // value and a whole number are both a 4-byte slot, so nothing but this flag distinguishes
    // them, and mixing the two silently compares numbers 65,536 apart in meaning.
    struct Local { const char* name; size_t nameLen; uint8_t slot; CtrlType type; };
    Local              locals[kMaxLocals] = {};
    uint8_t            localCount = 0;
    uint8_t            slotHighWater = 0;   // slots currently in scope
    uint8_t            slotsUsed = 0;       // PEAK slots — what the prologue reserves
    uint8_t            nextLabel = 0;          // IR label ids, handed out in source order

    // Every class-scope `uint8_t x = 0;` is a MEMBER: the class model, where a declaration inside
    // the class is a member of it. Whether the UI shows one is a separate question the script
    // answers by calling addControl in defineControls, so `controls` below is a VIEW of these rather
    // than a second storage: a control's offset IS its member's arena byte.
    //
    // `DeclaredControl` carries both because they are the same record. A member that no control
    // names simply never appears in the control list, and the binding creates no card for it.
    char*              strings = nullptr;      // the caller's pool; see compileSource
    uint16_t           stringCap = 0;
    uint16_t           stringLen = 0;
    DeclaredControl    members[kMaxCtrls] = {};
    // Arena bytes the members declared so far occupy: the cursor the next declaration is placed at.
    // Separate from memberCount now that a member's size is not always one byte.
    // THE TYPE OF THE VALUE JUST PARSED. The front end is a single-pass parser with no AST, so a
    // type rides alongside the register rather than hanging off a tree node: every parse function
    // sets this before returning, and the places where two values meet compare it.
    //
    // Only `fixed` is tracked distinctly. byte and bool DECAY to int the moment they are read —
    // they are semantics on storage, not on arithmetic — and a string never enters an expression.
    // So this answers one question: is this value Q16.16, or a plain integer?
    bool               exprIsFixed = false;
    // When the value just parsed is EXACTLY one integer literal, the index of its Const op;
    // -1 otherwise. This is what lets `v * 2` and `if (v < 0)` work on a fixed value: an integer
    // LITERAL meeting a fixed operand converts at compile time by patching the already-emitted
    // Const (the number the script wrote, rescaled — free at run time), where a fixed-meets-int
    // VARIABLE stays a compile error naming toFixed/toInt. The distinction is safety: a literal's
    // meaning is visible at the call site; a variable's scaling is not.
    int                exprLitConst = -1;

    uint8_t            memberBytes = 0;
    uint8_t            memberCount = 0;

    const char*        error = "";
    uint16_t           errorCol = 0;
    bool               failed = false;

    void fail(const char* msg) { if (!failed) { failed = true; error = msg; errorCol = lex.col(); } }


    /// Copy a token's text into the program's string pool and return a pointer to it.
    ///
    /// The source buffer is freed the moment the compile returns, so a pointer into it would
    /// dangle before the emitted code ran. The pool travels with the compiled program instead,
    /// which is the same lifetime answer the engine already gives control and entry-point names.
    /// Null when the pool is full, which the caller turns into a diagnostic rather than a silent
    /// truncation.
    const char* internString(const char* text, size_t len) {
        if (!strings || stringLen + len + 1 > stringCap) return nullptr;
        char* at = strings + stringLen;
        for (size_t i = 0; i < len; i++) at[i] = text[i];
        at[len] = '\0';
        stringLen = static_cast<uint16_t>(stringLen + len + 1);
        return at;
    }


    /// Find a script-local (a `for` counter or limit) by name; its index, or -1. Same span
    /// comparison as findMember, for the same reason.
    int findLocal(const char* name, size_t len) const {
        for (uint8_t i = 0; i < localCount; i++)
            if (locals[i].nameLen == len && std::strncmp(locals[i].name, name, len) == 0)
                return i;
        return -1;
    }

    /// Find a declared MEMBER by name; its index, or -1. Names are token spans into the source
    /// rather than NUL-terminated strings, so compare by length and bytes.
    /// Truncate `v` to the width `type` promises, in the value itself.
    ///
    /// A MEMBER narrows in its store: StoreCtrl writes one byte and the slot's upper three keep
    /// their zero. A frame slot has no such instruction (Spill/Reload are whole-slot), so a byte
    /// LOCAL would silently hold 300 where the member holds 44, and the same line would mean two
    /// different things depending on where the variable was declared. Shifting up and back down
    /// truncates with two ops every backend already has, which is also why it is spelled this way
    /// rather than with a mask: the IR has no bitwise AND.
    ///
    /// A bool truncates to a byte for the same reason the member path does: every use of a bool is
    /// a comparison, where any non-zero is true.
    void narrowToType(VReg v, CtrlType type) {
        if (type != CtrlType::Byte && type != CtrlType::Bool) return;
        emit({IrOp::Shl, v, v, 0,0,0, 24, nullptr, {}});
        emit({IrOp::Shr, v, v, 0,0,0, 24, nullptr, {}});
    }

    /// Whether a compile-time constant fits the range `type` promises.
    ///
    /// The same bounds a MEMBER's initializer is checked against, so `byte b = 300;` is refused
    /// wherever it is written rather than becoming 44 in one position and 300 in the other.
    static bool fitsInType(int64_t v, CtrlType type) {
        switch (type) {
            case CtrlType::Byte: return v >= 0 && v <= 255;
            case CtrlType::Bool: return v >= 0 && v <= 1;
            default:             return true;      // int and fixed take the whole slot
        }
    }

    int findMember(const char* name, size_t len) const {
        for (uint8_t i = 0; i < memberCount; i++)
            if (members[i].nameLen == len && std::strncmp(members[i].name, name, len) == 0)
                return i;
        return -1;
    }

    // A stack temp allocator: alloc() hands out a recycled vreg if one is free, else a fresh one;
    // free() returns a temp to the pool once its value has been consumed. This is what keeps a
    // multi-call statement (e.g. setRGB(random16(..), random16(..), random16(..), 0)) within the
    // small device register file — each call's argument temp dies and is reused for the next,
    // instead of the count growing without bound. The textbook tree-walk register stack.
    VReg alloc() {
        if (freeCount) return freeStack[--freeCount];
        if (nextTemp < kMaxVRegs) return nextTemp++;
        // Out of virtual registers — a statement deeper than the fixed file holds. Fail the
        // compile rather than aliasing the last vreg (which would silently produce wrong IR).
        fail("script too complex (out of registers)");
        return kFirstTemp;
    }
    void freeTemp(VReg v) {
        // No local-register guard is needed: a variable lives in a frame slot, and reading one
        // hands back an ordinary temp that the consumer owns. Every vreg reaching here is a temp.
        if (v >= kFirstTemp && freeCount < kMaxVRegs) freeStack[freeCount++] = v;
    }

    // Append an IR op, failing the compile if the program is full or names an out-of-budget
    // vreg (IrProgram::push validates both). Centralises the check so no call site forgets it.
    void emit(const IrInst& i) { if (!ir.push(i)) fail("script too large"); }

    bool expect(Tok t, const char* msg) {
        if (lex.kind != t) { fail(msg); return false; }
        lex.advance();
        return true;
    }

    // expr    := term { ("+" | "-") term }
    // term    := primary { "*" primary }
    // primary := number | ident | call | "(" expr ")"
    //
    // Precedence climbing, the textbook shape: each level consumes the tighter-binding one below
    // it, so `2 + 3 * 4` is 14 rather than 20 without any special case. Every operator lowers to IR
    // the three backends ALREADY have (Const/Add/Mul) — a - b is emitted as a + (b * -1), because
    // no ISA here has a subtract and Xtensa's add-immediate encodes only 1..15, so negating the
    // immediate would silently produce a wrong constant.
    /// Both operands of a binary operator must agree, and an INTEGER LITERAL meeting a fixed
    /// operand agrees by converting: its Const is patched to the same number in Q16.16, costing
    /// nothing at run time. Anything else mixed is a compile error naming the conversion to
    /// write, because the alternative is a number 65,536 times off with nothing reporting it: the
    /// two representations are indistinguishable at run time.
    ///
    /// Returns the type of the combined expression (true = fixed) via `outFixed`.
    bool meet(bool lhsFixed, int lhsLit, bool rhsFixed, int rhsLit, bool& outFixed) {
        if (lhsFixed == rhsFixed) { outFixed = lhsFixed; return true; }
        const int lit = lhsFixed ? rhsLit : lhsLit;    // the int side, if it is a bare literal
        if (lit >= 0) {
            const int32_t v = ir.ops[lit].imm;
            if (v < -32768 || v > 32767) {
                fail("this number is out of range for a fixed value");
                return false;
            }
            ir.ops[lit].imm = v << 16;
            outFixed = true;
            return true;
        }
        fail("mixes whole and fixed: write toFixed(x) or toInt(x)");
        return false;
    }

    VReg parseExpr() {
        VReg lhs = parseTerm();
        while (!failed && (lex.kind == Tok::Plus || lex.kind == Tok::Minus)) {
            const bool negate = (lex.kind == Tok::Minus);
            lex.advance();
            const bool lhsFixed = exprIsFixed;
            const int  lhsLit   = exprLitConst;
            VReg rhs = parseTerm();
            if (failed) return 0;
            bool outFixed = false;
            if (!meet(lhsFixed, lhsLit, exprIsFixed, exprLitConst, outFixed)) return 0;
            exprIsFixed = outFixed;
            exprLitConst = -1;                     // a combined value is no longer one literal
            if (negate) {
                VReg m = alloc();
                emit({IrOp::Const, m, 0,0,0,0, -1, nullptr, {}});
                VReg n = alloc();
                emit({IrOp::Mul, n, rhs, m, 0,0, 0, nullptr, {}});
                freeTemp(m); freeTemp(rhs);
                rhs = n;
            }
            VReg dst = alloc();
            emit({IrOp::Add, dst, lhs, rhs, 0,0, 0, nullptr, {}});
            freeTemp(lhs); freeTemp(rhs);
            lhs = dst;
        }
        return lhs;
    }

    // `a / b` and `a % b` lower to a HOST CALL, not to an instruction: the IR's whole arithmetic
    // set is Const/Add/AddImm/Mul because no ISA here has a divide (Xtensa has none at all), so a
    // divide is a software routine wherever it appears. The operator is therefore syntax over the
    // machinery `mod(a, b)` already uses and costs nothing extra: but it is a call PER USE, which
    // is cheap on a cold path and deliberate inside a per-pixel loop.
    //
    // Resolved by NAME through the builtin table rather than emitted directly, because core is
    // domain-neutral: it knows no function, only what the host registered. A domain that registers
    // no "div" simply has no '/' operator, and says so at compile time instead of miscompiling.
    VReg emitBinaryCall(const char* name, size_t nameLen, VReg lhs, VReg rhs, const char* absent) {
        const Builtin* fn = table.find(name, nameLen);
        if (!fn || fn->argc != 2 || !fn->returns || fn->kind != BuiltinKind::Call) {
            fail(absent); return 0;
        }
        // Both operands go through consecutive frame slots, exactly as a two-argument call stages
        // them: the Call op carries where they start and how many, and nothing stays in a register
        // across it.
        if (slotHighWater + 1 >= kMaxLocals) { fail("expression too deeply nested"); return 0; }
        const uint8_t argBase = slotHighWater;
        emit({IrOp::Spill, 0, lhs, 0,0,0, slotHighWater++, nullptr, {}});
        emit({IrOp::Spill, 0, rhs, 0,0,0, slotHighWater++, nullptr, {}});
        if (slotHighWater > slotsUsed) slotsUsed = slotHighWater;
        freeTemp(lhs); freeTemp(rhs);
        VReg dst = alloc();
        emit({IrOp::Call, dst, 0, 2, 0, 0, argBase, fn->fn, {}});
        slotHighWater = argBase;               // the staging slots die with the call
        return dst;
    }

    VReg parseTerm() {
        VReg lhs = parsePrimary();
        while (!failed && (lex.kind == Tok::Star || lex.kind == Tok::Slash
                           || lex.kind == Tok::Percent)) {
            const Tok op = lex.kind;
            const bool lhsFixed = exprIsFixed;
            const int  lhsLit   = exprLitConst;
            lex.advance();
            VReg rhs = parsePrimary();
            if (failed) return 0;
            bool bothFixed = false;
            if (!meet(lhsFixed, lhsLit, exprIsFixed, exprLitConst, bothFixed)) return 0;
            exprLitConst = -1;
            if (op == Tok::Star && bothFixed) {
                // Q16.16 * Q16.16 has THIRTY-TWO fraction bits, so the product has to come back
                // down by 16. The answer is the 64-bit product's middle word: the high half's low
                // 16 bits joined to the low half's top 16. Three instructions and no call, which
                // is why the backends grew mulhi rather than routing this through a host function.
                VReg hi = alloc();
                emit({IrOp::Mulhi, hi, lhs, rhs, 0,0, 0, nullptr, {}});
                VReg lo = alloc();
                emit({IrOp::Mul, lo, lhs, rhs, 0,0, 0, nullptr, {}});
                emit({IrOp::Shr, lo, lo, 0,0,0, 16, nullptr, {}});   // LOGICAL: the low word is unsigned
                emit({IrOp::Shl, hi, hi, 0,0,0, 16, nullptr, {}});
                VReg dst = alloc();
                emit({IrOp::Add, dst, hi, lo, 0,0, 0, nullptr, {}});
                freeTemp(hi); freeTemp(lo); freeTemp(lhs); freeTemp(rhs);
                lhs = dst;
                exprIsFixed = true;
            } else if (op == Tok::Star) {
                VReg dst = alloc();
                emit({IrOp::Mul, dst, lhs, rhs, 0,0, 0, nullptr, {}});
                freeTemp(lhs); freeTemp(rhs);
                lhs = dst;
                exprIsFixed = false;
            } else if (op == Tok::Slash && bothFixed) {
                // Q16.16 / Q16.16 goes through its own host call: the quotient needs the
                // numerator widened to (a << 16) BEFORE the divide, and no 32-bit register can
                // hold that past |128.0|. A first attempt split the shift around an integer
                // divide (8 before, 8 after) and silently wrapped for larger values, which froze
                // two shipped shaders. fdiv does the widening in int64 in the host — exact over
                // the whole range, and a divide is a host call on every ISA here anyway.
                lhs = emitBinaryCall("fdiv", 4, lhs, rhs, "'/' on fixed needs the fdiv built-in");
                exprIsFixed = true;
            } else if (op == Tok::Slash) {
                lhs = emitBinaryCall("div", 3, lhs, rhs, "'/' needs a div(a, b) built-in");
                exprIsFixed = false;
            } else {
                // (a*2^16) mod (b*2^16) IS (a mod b)*2^16, so the remainder of two fixed values
                // is fixed — which is what makes the fractional-part idiom `x % 1.0` work.
                lhs = emitBinaryCall("mod", 3, lhs, rhs, "'%' needs a mod(a, b) built-in");
                exprIsFixed = bothFixed;
            }
            if (failed) return 0;
        }
        return lhs;
    }

    // A bare ident that names a declared control reads its live value (a LoadCtrl of its arena
    // offset); an ident followed by `(` is a call.
    VReg parsePrimary() {
        if (failed) return 0;
        // An int unless something below says otherwise, and not a bare literal unless the number
        // branch says so. Set here rather than in each branch so a new kind of primary cannot
        // forget to answer either question.
        exprIsFixed = false;
        exprLitConst = -1;
        // toFixed(v) / toInt(v): the conversions, EXPLICIT because a silent one is a number
        // 65,536 times off. Each is a single shift, recognized here rather than registered as a
        // builtin so it costs an instruction and not a host call.
        if (lex.kind == Tok::Ident && (atKeyword("toFixed", 7) || atKeyword("toInt", 5))) {
            const bool up = atKeyword("toFixed", 7);
            lex.advance();
            if (!expect(Tok::LParen, "expected '(' after the conversion")) return 0;
            VReg v = parseExpr();
            if (failed) return 0;
            if (up && exprIsFixed) { fail("this value is already fixed"); return 0; }
            if (!up && !exprIsFixed) { fail("this value is already a whole number"); return 0; }
            // A LITERAL converting up is range-checked here, exactly as adoption checks one at a
            // meet point: `toFixed(40000)` would otherwise shift past what Q16.16 holds and wrap
            // into a number nobody wrote. A computed value cannot be checked at compile time and
            // saturates at run time like any other overflow.
            if (up && exprLitConst >= 0) {
                const int32_t lit = ir.ops[exprLitConst].imm;
                if (lit < -32768 || lit > 32767)
                    { fail("this number is out of range for a fixed value"); return 0; }
            }
            if (!expect(Tok::RParen, "expected ')' to close the conversion")) return 0;
            VReg dst = alloc();
            emit({up ? IrOp::Shl : IrOp::Sar, dst, v, 0,0,0, 16, nullptr, {}});
            freeTemp(v);
            exprIsFixed = up;
            return dst;
        }
        if (lex.kind == Tok::LParen) {                   // grouping
            lex.advance();
            VReg v = parseExpr();
            if (!expect(Tok::RParen, "expected ')'")) return 0;
            return v;
        }
        if (lex.kind == Tok::Minus) {                    // unary minus: 0 - v, as (v * -1)
            lex.advance();
            // A NEGATED LITERAL folds here, before the positive form is range-checked: the
            // magnitude 2147483648 is legal only with the sign attached, so parsing the number
            // first made INT32_MIN unwritable in an expression (`v = -2147483648;` was refused
            // while the identical initializer compiled). Folding also spares a Const and a Mul.
            if (lex.kind == Tok::Number) {
                const int64_t neg = -lex.number;
                if (neg < INT32_MIN || neg > INT32_MAX) { fail("number out of range"); return 0; }
                VReg v = alloc();
                emit({IrOp::Const, v, 0,0,0,0, static_cast<int32_t>(neg), nullptr, {}});
                // A FIXED literal folds here too: `-32768.0` is the most negative Q16.16 value and
                // its magnitude is one past the positive limit, so parsing the number first and
                // negating after made it unwritable — the same shape as INT32_MIN.
                exprIsFixed = lex.numberIsFixed;
                exprLitConst = lex.numberIsFixed ? -1 : int(ir.count) - 1;
                lex.advance();
                return v;
            }
            VReg v = parsePrimary();
            if (failed) return 0;
            VReg m = alloc();
            emit({IrOp::Const, m, 0,0,0,0, -1, nullptr, {}});
            VReg dst = alloc();
            emit({IrOp::Mul, dst, v, m, 0,0, 0, nullptr, {}});
            freeTemp(m); freeTemp(v);
            return dst;
        }
        // `true` and `false`, the way a bool is written. Ordinary literals rather than a separate
        // token kind: they evaluate to 1 and 0, so every existing comparison and arithmetic path
        // takes them unchanged, and a script says `bool on = true;` instead of spelling a C-ism.
        if (lex.kind == Tok::Ident && (atKeyword("true", 4) || atKeyword("false", 5))) {
            VReg v = alloc();
            emit({IrOp::Const, v, 0,0,0,0, atKeyword("true", 4) ? 1 : 0, nullptr, {}});
            lex.advance();
            return v;
        }
        if (lex.kind == Tok::Number) {
            // The whole signed range: a member holds 32 bits, so a literal that fits one is
            // legal. The old 0..65535 cap was the widest MEMBER of the day, which made a literal
            // and the member it was assigned to disagree about what a number could be.
            if (lex.number < INT32_MIN || lex.number > INT32_MAX)
                { fail("number out of range"); return 0; }
            // A positive fixed literal stops at 32767.99998; the magnitude 32768.0 the lexer now
            // admits is legal only with the minus the branch above folds.
            if (lex.numberIsFixed && lex.number > INT32_MAX)      // 32767.99998 in Q16.16
                { fail("number out of range for a fixed value"); return 0; }
            VReg v = alloc();
            emit({IrOp::Const, v, 0,0,0,0, static_cast<int32_t>(lex.number), nullptr, {}});
            // A decimal point made it a fixed value at the lexer; the word is already scaled.
            exprIsFixed = lex.numberIsFixed;
            // A bare integer literal may still ADOPT fixed at a meet point (see meet()), which
            // patches this very op. Recording the index here is what makes that possible.
            if (!lex.numberIsFixed) exprLitConst = int(ir.count) - 1;
            lex.advance();
            return v;
        }
        if (lex.kind == Tok::Ident) {
            // A system variable the host defines: `t` (elapsed ms, an argument register) or a
            // per-frame value the binding writes (`width`/`height`/`depth`, arena slots). Resolved
            // BEFORE locals and controls so the name means one thing in every script; the
            // declaration paths reject the name, so nothing can shadow it.
            if (const SysVar* sv = sysvars.find(lex.identBeg, lex.identLen)) {
                lex.advance();
                if (sv->kind == SysVarKind::Arg) {
                    VReg v = alloc();   // parked at entry — bring it back for this one read
                    emit({IrOp::Reload, v, 0,0,0,0, hostArgSlot(sv->where), nullptr, {}});
                    return v;
                }
                VReg v = alloc();
                // LoadCtrl32, not LoadCtrl: a system variable occupies a 4-byte slot because a
                // grid is wider than a byte. A 1-byte load read only the low byte, which on a
                // 768-wide wall is 0 for width and 102 for height.
                emit({IrOp::LoadCtrl32, v, 0,0,0,0, sv->where, nullptr, {}});
                return v;
            }
            const int li = findLocal(lex.identBeg, lex.identLen);
            if (li >= 0) {
                // Load the variable from its frame slot into a fresh temp. The temp is ordinary:
                // the caller consumes it and frees it like any other expression value, so a
                // variable occupies a register only for the instruction that reads it rather
                // than for the whole of its scope.
                lex.advance();
                VReg v = alloc();
                emit({IrOp::Reload, v, 0,0,0,0, locals[li].slot, nullptr, {}});
                exprIsFixed = (locals[li].type == CtrlType::Fixed);
                return v;
            }
            // A MEMBER read. A control is a member the UI shows, so this one lookup answers both:
            // the arena byte is the same byte either way.
            const int mi = findMember(lex.identBeg, lex.identLen);
            if (mi >= 0) {
                lex.advance();
                // An ARRAY element: `heat[i]`, where the index is an arbitrary expression. The
                // same orthogonality the rest of the language has, so an index may be a member, a
                // loop counter or arithmetic over both.
                if (lex.kind == Tok::LBracket) {
                    if (members[mi].count == 1) { fail("this member is not an array"); return 0; }
                    lex.advance();
                    VReg idx = parseExpr();
                    if (failed) return 0;
                    // An INDEX counts elements, so it is a whole number like a loop counter.
                    if (exprIsFixed) { fail("an array index is a whole number: write toInt(x)"); return 0; }
                    if (!expect(Tok::RBracket, "expected ']' to close an array index")) { freeTemp(idx); return 0; }
                    VReg v = alloc();
                    // The value this expression yields is an ELEMENT, so its type is the array's.
                    // Leaving the index's state here made `heat[3] * 0.5` adopt the INDEX literal
                    // — patching the 3 to 196608, clamping to the last element, and reading a byte
                    // as though it were fixed.
                    exprIsFixed = (members[mi].type == CtrlType::Fixed);
                    exprLitConst = -1;
                    emit({IrOp::LoadIdx, v, idx, 0, 0, 0,
                          idxPack(members[mi].offset, ctrlWidth(members[mi].type),
                                  members[mi].count), nullptr, {}});
                    freeTemp(idx);
                    return v;
                }
                if (members[mi].count > 1) { fail("an array needs an index: write name[i]"); return 0; }
                VReg v = alloc();
                exprIsFixed = (members[mi].type == CtrlType::Fixed);
                // ONE load for every scalar type. A slot is 4 bytes and already holds what its
                // type promises (byte and bool are masked on the way in), so there is no width or
                // sign to choose here: the three-way pick this replaces is exactly where a
                // sign-blind read used to turn -100 into 65436.
                emit({IrOp::LoadCtrl32, v, 0,0,0,0, members[mi].offset, nullptr, {}});
                return v;
            }
            VReg out = 0;
            const Builtin* called = table.find(lex.identBeg, lex.identLen);
            parseCall(&out);   // otherwise a call used as an expression must return a value
            // The result's type is the builtin's business: uvX/uvY hand back a fixed coordinate,
            // everything else a whole number.
            exprIsFixed = called && called->fixedReturn;
            exprLitConst = -1;
            return out;
        }
        fail("expected a number, a control name, or a function call");
        return 0;
    }

    // call := ident "(" [expr {"," expr}] ")".  If `resultOut` is non-null the call is used as
    // an expression and must return a value; the result vreg is written there. If null it is a
    // statement (void).
    void parseCall(VReg* resultOut) {
        if (lex.kind != Tok::Ident) { fail("expected a function name"); return; }
        const Builtin* fn = table.find(lex.identBeg, lex.identLen);
        if (!fn) {
            // Not a built-in: the script's own function, if it declared one by this name. Resolved
            // against the class's function list rather than the builtin table, which is what makes
            // a helper callable and, when the name is the running function's own, what makes
            // recursion work: nothing here treats the two cases differently.
            //
            // Only functions ALREADY PARSED are visible. A forward call (to a helper declared
            // further down) is refused rather than half-supported, because resolving it needs a
            // second pass over the class body. The cost is that "unknown function" is what a
            // forward call reports too, with the column but not the name, so a helper has to be
            // declared above its caller. Recursion is unaffected: a function is added to the list
            // before its body is parsed, so it can see itself.
            for (uint8_t i = 0; i < fnCount; i++) {
                if (fns[i].nameLen != lex.identLen) continue;
                if (std::strncmp(fns[i].name, lex.identBeg, lex.identLen) != 0) continue;
                lex.advance();
                if (!expect(Tok::LParen, "expected '(' after the function name")) return;
                // ARGUMENTS, evaluated into REGISTERS first and stored to the arena block only
                // once they all are. The store cannot follow each argument, because evaluating a
                // later one may itself contain a call (`paint(0, twice(21))`) whose own arguments
                // go through the same block: writing as we go had the inner call overwrite the
                // outer's first argument, and paint drew 21 where 0 belonged.
                //
                // Delaying the stores to the last possible moment is what makes ONE block enough:
                // between the final store and the call there is nothing left to run.
                const uint8_t want = fns[i].params;
                VReg staged[kMaxScriptArgs] = {};
                uint8_t got = 0;
                while (lex.kind != Tok::RParen && lex.kind != Tok::End) {
                    if (got > 0 && !expect(Tok::Comma, "expected ',' between arguments")) return;
                    const VReg a = parseExpr();
                    if (failed) return;
                    if (got < want && got < kMaxScriptArgs) staged[got] = a;
                    else freeTemp(a);
                    got++;
                }
                if (!expect(Tok::RParen, "expected ')' to close the arguments")) return;
                const uint8_t staging = got < want ? got : want;
                for (uint8_t a = 0; a < staging && a < kMaxScriptArgs; a++) {
                    emit({IrOp::StoreCtrl32, 0, staged[a], 0,0,0, scriptArgOffset(a), nullptr, {}});
                    freeTemp(staged[a]);
                }
                if (got != want) {
                    fail(got < want ? "too few arguments for this function"
                                    : "too many arguments for this function");
                    return;
                }
                // A call used as a VALUE takes what the callee returned. Refused when the callee
                // declares void, since there is nothing to take and the script would otherwise
                // read whatever the return register happened to hold.
                if (resultOut) {
                    if (fns[i].ret == RetType::Void) {
                        fail("this function returns nothing, so it cannot be used as a value"); return;
                    }
                    if (fns[i].ret == RetType::Str) {
                        fail("a string return cannot be used in an expression yet"); return;
                    }
                    // The value stays in its register: a script call preserves the caller's
                    // vregs (every backend's callLabel saves the pool around it, exactly as a
                    // builtin call does), so a second call in the same expression cannot overwrite
                    // it. The first version left the pool unsaved and `a() + b()` read 6.
                    const VReg v = alloc();
                    emit({IrOp::CallScript, v, 0, 1, 0, 0, static_cast<int32_t>(i), nullptr, {}});
                    *resultOut = v;
                    return;
                }
                // `imm` is the callee's FUNCTION NUMBER, not its position in the op array. An IR
                // index would be the more obvious choice and was the first one, but the spill pass
                // rewrites the array and every index past its first insertion shifts: the call then
                // named a position that no longer started a function, and the lowering opened the
                // next frame mid-statement. A function number survives any rewrite of the ops.
                emit({IrOp::CallScript, 0, 0,0,0,0, static_cast<int32_t>(i), nullptr, {}});
                return;
            }
            fail("unknown function"); return;
        }
        lex.advance();
        if (!expect(Tok::LParen, "expected '(' after the function name")) return;

        // Evaluate each argument, PARKING it in a frame slot as soon as it is finished.
        //
        // The op that consumes them reads every argument at once, so evaluating straight into vregs
        // keeps all four live simultaneously — and each argument's own sub-expression needs
        // registers on top of that. On Xtensa's ten that is what refused every looped effect: a
        // four-operand setRGB left nothing to compute the operands WITH. Staging through the frame
        // means only ONE argument occupies a register at a time; they come back together in the
        // reload just before the op, where the peak is exactly the operand count and nothing more.
        // Stage every argument into a CONSECUTIVE frame slot as it is evaluated. The call then
        // carries the slot BASE and the COUNT rather than the values, so how many arguments a
        // builtin takes is bounded by frame slots — `draw::line` wants seven — and only one
        // argument occupies a register at a time.
        const uint8_t argBase = slotHighWater;
        uint8_t n = 0;
        if (lex.kind != Tok::RParen) {
            while (true) {
                if (n >= fn->argc) { fail("too many arguments"); return; }
                // Two argument forms an ordinary expression cannot carry, both needed so a control
                // is declared by the same call a compiled module makes:
                //
                //   a STRING, for the UI label. A frame slot is a machine word, so the pointer
                //   into the source fits; the host reads it as a `const char*`.
                //
                //   a MEMBER BY NAME, meaning its ADDRESS rather than its value. `addControl("bpm",
                //   bpm, 1, 120)` reads as the reference a compiled module passes, and the compiler
                //   supplies the arena offset the host binds to. Only where the builtin asks for it
                //   (byRef), so `setRGB(bpm, …)` still reads bpm's value as it always did.
                VReg v = 0;
                const bool wantStr = (fn->byStr >> n) & 1u;
                if (wantStr && lex.kind != Tok::String) {
                    fail("this argument must be a name in quotes"); return;
                }
                // And a string ONLY where one is wanted: `setRGB("red", 0, 0, 0)` would otherwise
                // pass the low bits of a pointer as a color index.
                if (!wantStr && lex.kind == Tok::String) {
                    fail("this argument is a number, not a name in quotes"); return;
                }
                if (lex.kind == Tok::String) {
                    // The label is recorded HERE, at compile time, rather than travelling through
                    // a frame slot: the source buffer is freed after the compile, so a pointer the
                    // emitted code carried would dangle by the time the host read it. The engine
                    // already copies control names into its own pool, which is the same lifetime
                    // problem solved once. The slot still gets a value so the argument count is
                    // honest; the host reads the name from the control record, not from the slot.
                    // INTERNED, so the pointer outlives the source. The text is freed the moment
                    // the compile returns, and this pointer travels in the emitted code to a host
                    // that reads it later, so it cannot point into the source. The pool is part of
                    // the compiled program, exactly as the entry-point and control names already
                    // are: the same lifetime problem, solved the same way.
                    // Interned into the caller's pool, which outlives the compile, so the address
                    // is final the moment it is made and the emitted code carries it directly.
                    const char* interned = internString(lex.identBeg, lex.identLen);
                    if (!interned) { fail("no room for this script's strings"); return; }
                    v = alloc();
                    emit({IrOp::ConstPtr, v, 0,0,0,0, 0, nullptr, interned, {}});
                    lex.advance();
                } else if ((fn->byRef >> n) & 1u) {
                    if (lex.kind != Tok::Ident) { fail("expected the member this control is bound to"); return; }
                    const int mi = findMember(lex.identBeg, lex.identLen);
                    if (mi < 0) { fail("no member of that name is declared in this class"); return; }
                    // A control surfaces a value the UI can drive, so the member's type has to be
                    // one the UI has a widget for. int, byte and bool do; fixed and string do not
                    // yet, and saying so beats publishing a slider that writes a Q16.16 word the
                    // user cannot reason about. A control also drives one value, never an array:
                    // binding one would move element 0 and leave the rest, with nothing on screen
                    // saying so.
                    if (members[mi].type == CtrlType::Fixed || members[mi].type == CtrlType::Str)
                        { fail("a control binds an int, byte or bool member"); return; }
                    if (members[mi].count > 1)
                        { fail("a control binds a single member, not an array"); return; }
                    // The offset AND the member's type travel in one word: the arena is 64 bytes,
                    // so the low byte carries the offset and the next byte the type. addControl
                    // needs the type to know which widget to publish, and it has no other way to
                    // learn it — the builtin sees values, not declarations.
                    v = alloc();
                    emit({IrOp::Const, v, 0,0,0,0,
                          members[mi].offset | (int32_t(members[mi].type) << 8), nullptr, {}});
                    lex.advance();
                } else {
                    const bool wantFixed = ((fn->fixedArgs >> n) & 1u) != 0;
                    v = parseExpr();
                    if (failed) return;
                    // Each argument is checked against what the BUILTIN declares. Most take whole
                    // numbers — a channel, a light index, an angle16 — and a fixed value crossing
                    // uncoverted would read 65,536 times off. escape() is the exception: it takes
                    // four fixed coordinates, so uv output flows straight in.
                    if (wantFixed != exprIsFixed) {
                        bool adopted = false;
                        if (!wantFixed || !meet(true, -1, exprIsFixed, exprLitConst, adopted)) {
                            fail(wantFixed ? "this argument is a fixed value: write toFixed(x)"
                                           : "this argument is a whole number: write toInt(x)");
                            return;
                        }
                    }
                }
                if (failed) return;
                if (slotHighWater >= kMaxLocals) { fail("too many arguments to hold"); return; }
                emit({IrOp::Spill, 0, v, 0,0,0, slotHighWater++, nullptr, {}});
                freeTemp(v);                       // its register is free again immediately
                n++;
                if (lex.kind == Tok::Comma) { lex.advance(); continue; }
                break;
            }
        }
        if (slotHighWater > slotsUsed) slotsUsed = slotHighWater;
        if (n != fn->argc) { fail("wrong number of arguments"); return; }

        if (!expect(Tok::RParen, "expected ')'")) return;

        // The IR Call op carries a single argument vreg, so a Call-kind builtin must be unary.
        // (Today random16 is the only one.) Reject a multi-arg Call up front rather than silently
        // dropping args[1..]; a future N-ary helper needs the IR Call contract widened first.
        if (resultOut) {
            if (fn->kind != BuiltinKind::Call || !fn->returns) { fail("this function does not return a value"); return; }
            // The arguments live in the frame, so nothing is held in a register across the call:
            // `imm` carries the slot they start at and `b` how many there are.
            VReg r = alloc();
            emit({IrOp::Call, r, 0, n, 0, 0, argBase, fn->fn, {}});
            *resultOut = r;
        } else {
            // A statement call. Call kinds with a result are also allowed as statements (result
            // discarded); Inline kinds emit the inline op with their operands.
            if (fn->kind == BuiltinKind::Call) {
                VReg r = alloc();
                emit({IrOp::Call, r, 0, n, 0, 0, argBase, fn->fn, {}});
                freeTemp(r);
            } else {
                // An inline op reads its operands from REGISTERS (it is emitted as instructions, not
                // a call), so reload the staged arguments for the one op that consumes them.
                //
                // FOUR is the IrInst operand ceiling for an inline op. A builtin declaring more used
                // to be truncated here — the extra arguments evaluated, then silently dropped — which
                // emits a working-looking op that computes the wrong thing. Refuse instead: only a
                // CALL is unbounded (its arguments go through the frame), so a wider inline builtin
                // needs the IR widened first, not its arguments quietly discarded.
                if (n > 4) { fail("this function takes too many arguments to inline"); return; }
                VReg a0 = 0, a1 = 0, a2 = 0, a3 = 0;
                VReg* slot[4] = {&a0, &a1, &a2, &a3};
                for (uint8_t i = 0; i < n && i < 4; i++) {
                    *slot[i] = alloc();
                    emit({IrOp::Reload, *slot[i], 0,0,0,0, static_cast<int32_t>(argBase + i), nullptr, {}});
                }
                emit({IrOp::Inline, 0, a0, a1, a2, a3, 0, nullptr, nullptr, fn->inlineOp});
                for (uint8_t i = 0; i < n && i < 4; i++) freeTemp(*slot[i]);
            }
        }
        // Give the staging slots back — but only down to where THIS call started, and only once its
        // result has been produced. A nested call (`setRGB(1, mod(t, 200), …)`) stages inside its
        // parent's argument block, so resetting to a fixed base would hand the inner call the slots
        // the outer one is still filling: measured as mod()'s arguments landing on setRGB's, and its
        // result then overwriting them. `slotsUsed` keeps the peak, which is what the prologue
        // reserves, so releasing here costs nothing and lets sequential calls reuse the space.
        slotHighWater = argBase;
    }

    // A MEMBER declaration: `uint8_t ident = number ;`. Whether the UI shows it is a separate
    // question the script answers by naming it in defineControls().
    // The leading `uint8_t` keyword is already consumed by the caller. Records a DeclaredControl.
    void parseDecl(CtrlType type) {
        if (lex.kind != Tok::Ident) { fail("expected a member name after the type"); return; }
        const char* name = lex.identBeg; size_t nameLen = lex.identLen;
        if (nameLen >= kMaxControlName) { fail("member name too long"); return; }   // no silent truncation downstream
        if (sysvars.find(name, nameLen)) { fail("name is a system variable"); return; }
        // Against MEMBERS, which is where a declaration now lands. Checked against the controls
        // before, which stopped catching anything the moment a declaration became a member: two
        // members of one name would both exist, and every read would resolve to the first while
        // the second silently owned an arena byte nobody could reach.
        if (findMember(name, nameLen) >= 0) { fail("duplicate member name"); return; }
        // A control name must not shadow a builtin: a declared `random16` would make `random16(…)`
        // ambiguous (control read vs call). Reject it at the source so the resolution never collides.
        if (table.find(name, nameLen)) { fail("member name shadows a built-in function"); return; }
        // The conversions and the boolean literals are resolved BEFORE a member name, so a member
        // called one of these could be declared and then never read. Refused for the same reason a
        // builtin name is: the alternative is a member that silently does not exist.
        if (isReservedWord(name, nameLen)) { fail("member name is a reserved word"); return; }
        lex.advance();
        // An ARRAY: `byte heat[16];`. The length is a literal, not an expression, because the
        // arena is sized at compile time: a length read from a control would make the member's
        // size depend on a value the UI changes while the program runs.
        uint8_t count = 1;
        if (lex.kind == Tok::LBracket) {
            // A string is a reference into the compiled program's pool, so an array of them would
            // be an array of references with no way to fill it: there is no runtime string.
            if (type == CtrlType::Str) { fail("string arrays are not supported"); return; }
            // A fixed ARRAY waits for element type-tracking to be worth having: the element's
            // type has to reach the expression that reads it and the value that writes it, which
            // scalars get from their declaration and elements would need per-array. Refused
            // rather than half-working, the same stance string arrays take.
            if (type == CtrlType::Fixed) { fail("fixed arrays are not supported yet"); return; }
            lex.advance();
            if (lex.kind != Tok::Number) { fail("expected an array length (a number)"); return; }
            if (lex.number < 1 || lex.number > kCtrlBytes) { fail("array length out of range"); return; }
            count = static_cast<uint8_t>(lex.number);
            lex.advance();
            if (!expect(Tok::RBracket, "expected ']' to close the array length")) return;
            if (!expect(Tok::Semicolon, "expected ';': an array has no initializer")) return;
            if (lex.kind == Tok::Error) { fail(lex.err); return; }
            if (memberCount >= kMaxCtrls) { fail("too many members"); return; }
            // Elements pack at their own width — a byte[] is one byte each, which is what keeps a
            // heat map at 1x on a board with no PSRAM — but the array STARTS on a 4-byte boundary
            // so the scalar slots around it stay aligned.
            const uint8_t elem = ctrlWidth(type);
            uint16_t at = memberBytes;
            if ((at % 4) != 0) at = uint16_t(at + (4 - at % 4));
            const uint16_t need = uint16_t(count) * elem;
            if (at + need > kCtrlBytes) { fail("the class declares more member data than the arena holds"); return; }
            // Zero, not a written initializer: an element-wise initializer list would be a second
            // syntax for what a `for` in the script already expresses, and every element seeding to
            // the same value is what a decay or particle buffer starts from anyway.
            // The RANGE is not the compiler's to state: it arrives with the addControl call at
            // run time, through addDeclaredControl. Zeroed here rather than carrying a 0..255
            // that an int or fixed member would flatly contradict.
            members[memberCount] = {name, 0, 0, 0, static_cast<uint8_t>(nameLen), type,
                                    static_cast<uint8_t>(at), count};
            memberBytes = static_cast<uint8_t>(at + need);
            memberCount++;
            return;
        }
        if (!expect(Tok::Assign, "expected '=' in a member declaration")) return;
        // A leading minus, here and nowhere else in the grammar: an int16_t member's whole purpose
        // is to hold a negative, and `int16_t v = -100;` is how every language spells that. In an
        // EXPRESSION the same minus is the subtraction operator, which is why this is read at the
        // one point where a number is the only thing that can follow.
        bool negated = false;
        if (lex.kind == Tok::Minus) { negated = true; lex.advance(); }
        // `true`/`false` seed a bool the way a script writes one.
        if (lex.kind == Tok::Ident && (atKeyword("true", 4) || atKeyword("false", 5))) {
            if (type != CtrlType::Bool) { fail("true and false initialize a bool member"); return; }
            // `-true` parsed: the minus was consumed above and then never consulted, so the member
            // seeded to 1 as though the sign had not been written. A sign has no meaning on a
            // boolean, so say so rather than silently discarding it.
            if (negated) { fail("true and false take no sign"); return; }
            const long b = atKeyword("true", 4) ? 1 : 0;
            lex.advance();
            if (!expect(Tok::Semicolon, "expected ';' after the member declaration")) return;
            if (lex.kind == Tok::Error) { fail(lex.err); return; }
            if (memberCount >= kMaxCtrls) { fail("too many members"); return; }
            uint16_t bat = memberBytes;
            if ((bat % 4) != 0) bat = uint16_t(bat + (4 - bat % 4));
            if (bat + 4 > kCtrlBytes)
                { fail("the class declares more member data than the arena holds"); return; }
            members[memberCount] = {name, 0, 1, static_cast<int32_t>(b),
                                    static_cast<uint8_t>(nameLen), type,
                                    static_cast<uint8_t>(bat), 1};
            memberBytes = static_cast<uint8_t>(bat + 4);
            memberCount++;
            return;
        }
        // A quoted initializer reaches here only for a string member; say what is actually true
        // rather than asking for a number, which the string case then refuses in the other
        // direction. The two messages used to point at each other.
        if (lex.kind == Tok::String) { fail("a string member cannot be initialized yet"); return; }
        if (lex.kind != Tok::Number) { fail("expected a default value (a number)"); return; }
        if (negated) lex.number = -lex.number;
        // Range-checked against the DECLARED type, so a member cannot be given a value it would
        // silently truncate. `byte n = 300;` is refused here rather than becoming 44 at run time,
        // which is the class of bug this type system exists to remove: the old widths turned an
        // out-of-range initializer into an arbitrary in-range one with nothing reporting it.
        int64_t defMin = INT32_MIN, defMax = INT32_MAX;
        const char* rangeErr = nullptr;
        switch (type) {
            case CtrlType::Byte:  defMin = 0; defMax = 255;
                                  rangeErr = "byte default out of range (0..255)";
                                  if (lex.numberIsFixed)
                                      { fail("a byte member takes a whole number"); return; }
                                  break;
            case CtrlType::Bool:  defMin = 0; defMax = 1;
                                  rangeErr = "bool default is 0 or 1";
                                  if (lex.numberIsFixed)
                                      { fail("a bool member takes true or false"); return; }
                                  break;
            case CtrlType::Str:   fail("a string member cannot be initialized yet"); return;
            case CtrlType::Int:   rangeErr = "default out of range";
                                  if (lex.numberIsFixed)
                                      { fail("an int member takes a whole number"); return; }
                                  break;
            case CtrlType::Fixed:
                // A whole number seeding a fixed member is converted HERE, at compile time: the
                // script writes `fixed zoom = 2;` and means 2.0, and no runtime shift is spent on
                // a constant. A decimal literal arrived scaled already.
                if (!lex.numberIsFixed) {
                    if (lex.number < -32768 || lex.number > 32767)
                        { fail("fixed default out of range (-32768.0..32767.99998)"); return; }
                    lex.number = lex.number << 16;
                }
                rangeErr = "fixed default out of range (-32768.0..32767.99998)";
                break;
        }
        if (lex.number < defMin || lex.number > defMax) { fail(rangeErr); return; }
        int64_t def = lex.number;
        lex.advance();
        if (!expect(Tok::Semicolon, "expected ';' after the member declaration")) return;
        // A lexer error carries a specific message; surface it rather than letting it fall through
        // to a generic later parse failure.
        if (lex.kind == Tok::Error) { fail(lex.err); return; }
        // A MEMBER, and only that. Whether the UI shows it is a separate question the script
        // answers by naming it in `defineControls()`, so a declaration no longer carries a range:
        // the range belongs to the control, and a member that no control surfaces has none.
        if (memberCount >= kMaxCtrls) { fail("too many members"); return; }
        // The offset is a running BYTE CURSOR, not the declaration index: a member wider than a
        // byte consumes several, so the n-th member is no longer at byte n. Checked against the
        // arena's byte budget rather than against the record count, because those are now two
        // different limits and a script can exhaust either one first.
        // Every scalar takes one 4-byte SLOT on a 4-byte boundary, whatever its type. That is the
        // whole storage rule: no per-type width to align to, and the backends' 32-bit load and
        // store scale their immediate by 4, which every arena offset already satisfies.
        uint16_t at = memberBytes;
        if ((at % 4) != 0) at = uint16_t(at + (4 - at % 4));
        const uint16_t need = ctrlSlotBytes(type);
        if (at + need > kCtrlBytes) { fail("the class declares more member data than the arena holds"); return; }
        // def is int32_t on the record so a member's whole initializer survives; casting it
        // narrower here truncated `uint16_t phase = 1000;` to 232. Invisible to a test that
        // observes through setRGB, because the error is always a multiple of 256.
        // Range zeroed for the reason the array path gives: the control's range comes from
        // addControl at run time, not from the declaration.
        members[memberCount] = {name, 0, 0, static_cast<int32_t>(def),
                                static_cast<uint8_t>(nameLen), type,
                                static_cast<uint8_t>(at), 1};
        memberBytes = static_cast<uint8_t>(at + need);
        memberCount++;
    }

    /// The words the expression parser resolves before it looks for a member: the two conversions
    /// and the two boolean literals. A member may not take one of these names.
    static bool isReservedWord(const char* n, size_t len) {
        static const struct { const char* w; size_t len; } kWords[] = {
            {"toFixed", 7}, {"toInt", 5}, {"true", 4}, {"false", 5},
            // The type keywords too: `int int = 5;` parsed, declaring a member whose name the
            // class-body loop reads as the start of another declaration.
            {"int", 3}, {"byte", 4}, {"bool", 4}, {"fixed", 5}, {"string", 6},
            // And the statement keywords parseStatement dispatches on. `int if = 0;` bound a
            // variable to the word that OPENS a conditional, so every later `if` in the function
            // parsed as a reference to it and the diagnostic pointed at the wrong line.
            {"if", 2}, {"else", 4}, {"for", 3}, {"return", 6}, {"class", 5}, {"void", 4}};
        for (const auto& k : kWords)
            if (len == k.len && std::strncmp(n, k.w, k.len) == 0) return true;
        return false;
    }

    // Is the current Ident this exact keyword? Keywords are matched by text rather than lexed as
    // their own token kind: the set is tiny, and a script may still use `class` or `for` as part of
    // a longer identifier, which a length-checked compare gets right for free.
    bool atKeyword(const char* kw, size_t len) const {
        return lex.kind == Tok::Ident && lex.identLen == len && std::strncmp(lex.identBeg, kw, len) == 0;
    }
    // The five type keywords. Names a script author would reach for, not the storage widths the
    // language used to make them spell: a type says what a value MEANS, and the slot it occupies
    // is the compiler's business.
    bool atTypeKeyword() const {
        return atKeyword("int", 3) || atKeyword("byte", 4) || atKeyword("bool", 4) ||
               atKeyword("fixed", 5) || atKeyword("string", 6);
    }
    /// A function's declared return type: `void`, `int` or `string`. Deliberately NOT every member
    /// type: `byte tick()` would suggest the engine narrows the value, which it does not, and
    /// `fixed` is an int at this boundary. Three words, each meaning exactly one thing.
    bool atRetKeyword() const {
        return atKeyword("void", 4) || atKeyword("int", 3) || atKeyword("string", 6);
    }
    RetType currentRet() const {
        if (atKeyword("int", 3))    return RetType::Int;
        if (atKeyword("string", 6)) return RetType::Str;
        return RetType::Void;
    }
    /// The type the current keyword names. Only called when atTypeKeyword() is true.
    CtrlType currentType() const {
        if (atKeyword("byte", 4))   return CtrlType::Byte;
        if (atKeyword("bool", 4))   return CtrlType::Bool;
        if (atKeyword("fixed", 5))  return CtrlType::Fixed;
        if (atKeyword("string", 6)) return CtrlType::Str;
        return CtrlType::Int;
    }

    // program := { decl } { stmt }.  Declarations (control vars) come first, then one-or-more
    // call statements. (Multi-statement now: a script has decl lines AND a statement line.)
    /// stmt := call ";" | forStmt
    /// forStmt := "for" "(" "int" ident "=" expr ";" ident "<" expr ";" ident "=" expr ")" "{" {stmt} "}"
    ///
    /// C-style deliberately: it is the form a script author already knows, and the third clause is
    /// what a serpentine layout needs (`i = i + 2`, or counting down) without inventing more syntax.
    ///
    /// Lowered as a BOTTOM-TESTED loop, which is the shape FillElems has always emitted by hand and
    /// so is proven on all three ISAs:
    ///
    ///     i = init
    ///     BranchGe i, limit -> done      ; an empty range runs the body zero times
    ///   top:
    ///     body
    ///     i = step
    ///     BranchNe i, limit -> top       ; back edge
    ///   done:
    ///
    /// The back edge tests NOT-EQUAL rather than less-than because no ISA here has branch-if-less;
    /// that is exact for the `i = i + 1` case and terminates for any step that eventually hits the
    /// limit. A step that overshoots (`i = i + 3` over a limit it skips past) would not — so the
    /// limit is re-tested with BranchGe at the top of each iteration instead. See below.
    bool parseFor() {
        lex.advance();                                     // consume `for`
        if (!expect(Tok::LParen, "expected '(' after for")) return false;

        // --- init: "int" ident = expr ---
        // The counter is DECLARED, like every other variable in the language: a member carries its
        // type and an assignment to an undeclared name is refused, so a counter that appeared out of
        // nowhere was the one exception left. `int` is also the only type it could be (a fixed init
        // is rejected below), so the word adds no meaning for the compiler: it is here because it is
        // what C++ writes and what a script author types by habit.
        if (!atKeyword("int", 3)) {
            fail("a loop counter is declared: for (int i = 0; ...)");
            return false;
        }
        lex.advance();                                     // consume `int`
        if (lex.kind != Tok::Ident) { fail("expected a loop variable"); return false; }
        // Two slots per loop: the counter and the limit. Both must outlive the body, and both live
        // in the frame — nesting depth is now bounded by frame slots, not by the register file.
        if (localCount + 2 > kMaxLocals) { fail("too many nested loops"); return false; }
        const char* varName = lex.identBeg;
        const size_t varLen = lex.identLen;
        if (sysvars.find(varName, varLen)) { fail("name is a system variable"); return false; }
        // A nested loop reusing the enclosing loop's name would bind a SECOND register to that name:
        // the inner step then writes the register the outer back edge tests, and the emitted program
        // never terminates — a hang on the render task from a script a user can type. Refused for the
        // same reason a duplicate control name is.
        if (findLocal(varName, varLen) >= 0) { fail("loop variable already in use"); return false; }
        lex.advance();
        if (!expect(Tok::Assign, "expected '=' in the for's first clause")) return false;
        VReg init = parseExpr();
        // the init: A loop COUNTS, so every clause of its header is a whole number. Without this, a fixed limit runs the body ~65,536 times: a multi-second stall on the render thread rather than a diagnostic.
        if (exprIsFixed) { fail("a loop counts in whole numbers: write toInt(x)"); return false; }
        if (failed) return false;
        // The counter starts life in its slot; the temp that computed it is released immediately.
        // Bounded on slotHighWater, not just localCount: a call RELEASES its argument staging slots
        // (slotHighWater = argBase) without ever having counted them as locals, so the two can
        // diverge and the localCount check above is not sufficient on its own.
        if (slotHighWater >= kMaxLocals) { fail("too many loop variables"); return false; }
        const uint8_t counterSlot = slotHighWater++;
        emit({IrOp::Spill, 0, init, 0,0,0, counterSlot, nullptr, {}});
        freeTemp(init);
        const uint8_t myLocal = localCount;
        locals[localCount++] = {varName, varLen, counterSlot, CtrlType::Int};   // a counter counts
        if (!expect(Tok::Semicolon, "expected ';' after the for's first clause")) return false;

        // --- condition: ident < expr  (the only comparison the language has) ---
        // The name must be the loop variable: the emitted code tests `counter` whatever is written
        // here, so a different name compiles clean and runs as if it said the right one. That is a
        // wrong fixture with no diagnostic anywhere — the failure mode hardest to trace back to a
        // typo. (`for (y…) { for (x = 0; y < cols; x = x + 1) … }` is the realistic version.)
        if (lex.kind != Tok::Ident) { fail("expected the loop variable in the condition"); return false; }
        if (lex.identLen != varLen || std::strncmp(lex.identBeg, varName, varLen) != 0) {
            fail("the condition must test the loop variable"); return false;
        }
        lex.advance();
        if (!expect(Tok::Less, "expected '<' — it is the only comparison a for condition takes")) return false;
        // The bound goes to its own slot: it is read at the entry guard and again at the back edge,
        // so it has to survive the body — and a body containing a call would otherwise have to keep
        // it in a register across that call.
        VReg limitTmp = parseExpr();
        if (exprIsFixed) { fail("a loop counts in whole numbers: write toInt(x)"); return false; }
        if (failed) return false;
        if (slotHighWater >= kMaxLocals) { fail("too many loop variables"); return false; }
        const uint8_t limitSlot = slotHighWater++;
        emit({IrOp::Spill, 0, limitTmp, 0,0,0, limitSlot, nullptr, {}});
        freeTemp(limitTmp);
        if (!expect(Tok::Semicolon, "expected ';' after the for's condition")) return false;

        // --- step: ident = expr (parsed now, emitted after the body) ---
        if (lex.kind != Tok::Ident) { fail("expected the loop variable in the step"); return false; }
        if (lex.identLen != varLen || std::strncmp(lex.identBeg, varName, varLen) != 0) {
            fail("the step must advance the loop variable"); return false;   // it advances `counter` regardless
        }
        lex.advance();
        if (!expect(Tok::Assign, "expected '=' in the for's third clause")) return false;
        const char* stepSrc = lex.tokBeg;                  // re-lexed after the body
        // Skip the step expression without emitting: scan to the closing ')'.
        int depth = 0;
        while (!failed && lex.kind != Tok::End) {
            // A lexer error stops the scan. Tok::Error is not Tok::End and advance() does not move
            // past the offending character, so without this the loop spins forever on a script with
            // a stray character in the step expression — a hang, not a diagnostic.
            if (lex.kind == Tok::Error) { fail(lex.err); return false; }
            if (lex.kind == Tok::LParen) depth++;
            else if (lex.kind == Tok::RParen) { if (depth == 0) break; depth--; }
            lex.advance();
        }
        if (!expect(Tok::RParen, "expected ')' to close the for")) return false;
        if (!expect(Tok::LBrace, "expected '{' — a for's body is braced")) return false;

        if (nextLabel + 2 > kIrLabels) { fail("too many loops in one script"); return false; }
        const uint8_t lDone = nextLabel++;
        const uint8_t lTop  = nextLabel++;

        // Each test reloads both operands: they live in the frame, so a comparison is
        // load-load-branch. The reload temps die immediately, which is what keeps the body's
        // register demand independent of how deeply loops nest.
        {
            VReg c = alloc(), l = alloc();
            emit({IrOp::Reload, c, 0,0,0,0, counterSlot, nullptr, {}});
            emit({IrOp::Reload, l, 0,0,0,0, limitSlot,   nullptr, {}});
            emit({IrOp::BranchGe, 0, c, l, 0,0, lDone, nullptr, {}});   // empty range
            freeTemp(l); freeTemp(c);
        }
        emit({IrOp::Label,    0, 0,0,0,0,             lTop,  nullptr, {}});

        while (!failed && lex.kind != Tok::RBrace && lex.kind != Tok::End) {
            if (!parseStatement()) return false;
        }
        if (!expect(Tok::RBrace, "expected '}' to close the for's body")) return false;

        // The step, re-lexed from the source it was skipped over.
        {
            Lexer stepLex(stepSrc);
            Lexer save = lex;
            lex = stepLex;
            VReg s = parseExpr();
            if (exprIsFixed) { fail("a loop counts in whole numbers: write toInt(x)"); return false; }
            if (failed) return false;
            // parseExpr stops at the first token it cannot consume, so without this the step
            // silently ignores whatever follows it — `i = i + 1 garbage` compiled clean. The
            // skip-scan above already found the real ')', so anything else here is a typo.
            if (lex.kind != Tok::RParen) {
                lex = save; fail("unexpected token in the for's step"); return false;
            }
            emit({IrOp::Spill, 0, s, 0,0,0, counterSlot, nullptr, {}});
            freeTemp(s);
            lex = save;
        }
        // Re-test the limit at the top rather than relying on equality alone: a step that jumps
        // PAST the limit would never make counter == limit, and the loop would run away.
        {
            VReg c = alloc(), l = alloc();
            emit({IrOp::Reload, c, 0,0,0,0, counterSlot, nullptr, {}});
            emit({IrOp::Reload, l, 0,0,0,0, limitSlot,   nullptr, {}});
            emit({IrOp::BranchGe, 0, c, l, 0,0, lDone, nullptr, {}});
            emit({IrOp::BranchNe, 0, c, l, 0,0, lTop,  nullptr, {}});
            freeTemp(l); freeTemp(c);
        }
        emit({IrOp::Label,    0, 0,0,0,0,             lDone, nullptr, {}});

        localCount = myLocal;                              // the loop variable leaves scope
        // ...and its two SLOTS are returned, so sequential loops reuse the same frame space instead
        // of each costing its own for the rest of the program. Nesting still stacks, which is what
        // makes a script bounded by frame size rather than by register count. `slotsUsed` keeps the
        // PEAK, because that is what the prologue has to reserve.
        if (slotHighWater > slotsUsed) slotsUsed = slotHighWater;
        slotHighWater = counterSlot;
        return true;
    }

    /// One statement: a call, or a for.
    /// `name = expr;` is the statement that makes a member STATE rather than a constant.
    ///
    /// What may be assigned to is the rule worth stating. A MEMBER may: it is the script's own
    /// storage, and its arena byte surviving every call is exactly what a stateful effect (fire,
    /// trails, decay) needs. A script-LOCAL may: a `for` counter already lives in a frame slot and
    /// the step clause already writes it, so a body assignment is the same store the loop makes.
    ///
    /// A SYSTEM VARIABLE may not: the engine rewrites it before every call, so a store would
    /// appear to work and then vanish, which is worse than being refused.
    ///
    /// A CONTROL is deliberately NOT refused, though the UI does own its value. Whether a member
    /// becomes a control is decided at RUN time, by `defineControls()` calling `addControl` on it,
    /// so the parser cannot know: that is the direct consequence of a control being an ordinary
    /// call rather than an annotation. Writing one is also legitimate (an effect that ramps its
    /// own speed and lets the slider re-take it), and the outcome is visible rather than silent:
    /// the value moves under the slider until the user drags it again. The name is already
    /// consumed when this runs.
    bool parseAssignment(const char* name, size_t nameLen) {
        // An ARRAY element assignment: `heat[i] = expr;`. Resolved before the '=' because the
        // index sits between the name and the operator.
        if (lex.kind == Tok::LBracket) {
            const int ai = findMember(name, nameLen);
            if (ai < 0) { fail("no member of that name is declared in this class"); return false; }
            if (members[ai].count == 1) { fail("this member is not an array"); return false; }
            lex.advance();
            VReg idx = parseExpr();
            if (failed) return false;
            if (exprIsFixed) { fail("an array index is a whole number: write toInt(x)"); return false; }
            if (!expect(Tok::RBracket, "expected ']' to close an array index")) { freeTemp(idx); return false; }
            if (!expect(Tok::Assign, "expected '=' in an assignment")) { freeTemp(idx); return false; }
            VReg v = parseExpr();
            if (failed) { freeTemp(idx); return false; }
            // The same wall the scalar store enforces: an element takes what its type holds, or
            // the stored word is scaled 65,536 away from what the script meant. A literal adopts
            // for a fixed[] element exactly as it does at a binary operator.
            {
                const bool wantFixed = (members[ai].type == CtrlType::Fixed);
                if (wantFixed != exprIsFixed) {
                    bool adopted = false;
                    if (!wantFixed || !meet(true, -1, exprIsFixed, exprLitConst, adopted)) {
                        fail(wantFixed ? "a fixed element takes a fixed value: write toFixed(x)"
                                       : "this element takes a whole number: write toInt(x)");
                        return false;
                    }
                }
            }
            emit({IrOp::StoreIdx, 0, idx, v, 0, 0,
                  idxPack(members[ai].offset, ctrlWidth(members[ai].type),
                          members[ai].count), nullptr, {}});
            freeTemp(v); freeTemp(idx);
            return expect(Tok::Semicolon, "expected ';' after an assignment");
        }
        if (!expect(Tok::Assign, "expected '=' in an assignment")) return false;

        // Resolve the DESTINATION before parsing the value, so a refused target reports the name
        // the script wrote rather than a diagnostic from somewhere inside the expression.
        const int li = findLocal(name, nameLen);
        const int mi = li >= 0 ? -1 : findMember(name, nameLen);
        if (mi >= 0 && members[mi].count > 1) {
            fail("assign one element: name[i] = value");
            return false;
        }
        if (li < 0 && mi < 0) {
            if (sysvars.find(name, nameLen)) {
                fail("a system variable is read-only");
            } else {
                fail("not declared: add it to the class body");
            }
            return false;
        }
        VReg v = parseExpr();
        if (failed) return false;
        // The wall holds at the STORE too: a fixed member takes a fixed value and every other
        // member a whole number, or the slot would hold bits scaled 65,536 away from what the
        // script meant. Locals are loop counters, always whole numbers.
        if (mi >= 0) {
            const bool wantFixed = (members[mi].type == CtrlType::Fixed);
            if (wantFixed != exprIsFixed) {
                // `c = 5;` on a fixed member converts the literal at compile time, exactly as the
                // initializer does; anything computed still names its conversion.
                bool adopted = false;
                if (wantFixed && !meet(true, -1, exprIsFixed, exprLitConst, adopted) ) return false;
                if (!wantFixed) {
                    fail("this member takes a whole number: write toInt(x)");
                    return false;
                }
            }
        } else if ((locals[li].type == CtrlType::Fixed) != exprIsFixed) {
            // Same wall as a member, and for the same reason: the slot holds raw bits, so a scaling
            // mismatch is invisible at run time. A literal adopts the fixed side (`d = 5;` on a
            // fixed local), which is what keeps the arithmetic readable; anything computed names
            // its own conversion.
            bool adopted = false;
            if (locals[li].type == CtrlType::Fixed) {
                if (!meet(true, -1, exprIsFixed, exprLitConst, adopted)) return false;
            } else {
                fail("this variable takes a whole number: write toInt(x)");
                return false;
            }
        }
        if (li >= 0) narrowToType(v, locals[li].type);   // a byte local wraps at 255, like a member
        if (li >= 0) emit({IrOp::Spill,     0, v, 0,0,0, locals[li].slot,   nullptr, {}});
        // The store NARROWS: a byte or bool member takes StoreCtrl, which writes one byte, so the
        // value truncates in the instruction itself and the slot's upper three bytes keep the zero
        // they were seeded with. Nothing else can ever write them, which is what lets a byte
        // control's descriptor point at the slot's low byte and still see the member's whole
        // value. int and fixed take StoreCtrl32 and the whole slot. Same reasoning the ARRAY path
        // has always used, where store8 into a byte[] element narrows the same way.
        else         emit({storeOpFor(members[mi].type), 0, v, 0,0,0,
                           members[mi].offset, nullptr, {}});
        freeTemp(v);
        return expect(Tok::Semicolon, "expected ';' after an assignment");
    }

    /// The store op a member's type needs.
    ///
    /// A byte and a bool are NARROWED BY THE STORE ITSELF: StoreCtrl writes one byte, so the
    /// value truncates in the instruction and the slot's upper three bytes keep the zero they
    /// were seeded with. Nothing else can ever write them, which is what lets a byte control's
    /// descriptor point at the slot's low byte and read the member's whole value. The array path
    /// has always narrowed this way (store8 into a byte[] element); this makes a scalar match.
    ///
    /// int and fixed take the whole slot, so they store all four bytes.
    ///
    /// A bool truncates the same way, so it holds 0..255 rather than strictly 0 or 1: a script
    /// writing `flag = 7` reads 7 back, and every use of a bool is a comparison, where any
    /// non-zero is true. The one value that does NOT survive is a multiple of 256, which
    /// truncates to 0 — `flag = count` where count is 256 reads false. Normalizing would need a
    /// compare-and-select the IR has no op for (there is no bitwise op and no Sub), so it waits
    /// for a script that writes a non-boolean expression into a bool.
    static IrOp storeOpFor(CtrlType type) {
        return ctrlWidth(type) == 1 ? IrOp::StoreCtrl : IrOp::StoreCtrl32;
    }

    /// `if (a OP b) { … }` with an optional `else { … }`.
    ///
    /// The six comparisons lower onto the TWO branch ops the loops already use. The emitted branch
    /// skips the then-block, so each one emits the NEGATION of what the script wrote. `BranchGeS`
    /// is SIGNED `a >= b` and `BranchNe` is `a != b`, which is all this needs:
    ///
    /// SIGNED because this is the script's own comparison. The unsigned `BranchGe` still carries
    /// the constructs that COUNT rather than compare: a for loop's entry guard and back edge, the
    /// array-index clamp, and the `z >= z` unconditional-jump idiom below. See IrOp::BranchGe.
    ///
    ///     written    skip when    emitted
    ///     a <  b     a >= b       BranchGe a, b
    ///     a >  b     a <= b       BranchGe b, a          (b >= a is exactly a <= b)
    ///     a >= b     a <  b       emitStrictLess(a, b)   (strict, so not a single BranchGe)
    ///     a <= b     a >  b       emitStrictLess(b, a)
    ///     a == b     a != b       BranchNe a, b
    ///     a != b     a == b       BranchNe over an always-taken skip (there is no BranchEq)
    ///
    /// The branch always jumps AROUND the taken block, which is the shape that makes an `if` emit
    /// only FORWARD branches. That matters beyond tidiness: the spill pass identifies a loop as a
    /// BranchNe whose label was bound EARLIER, so a forward-only construct cannot be mistaken for
    /// one, and no change to the allocator is needed to support conditionals.
    bool parseIf() {
        lex.advance();   // `if`
        if (!expect(Tok::LParen, "expected '(' after if")) return false;
        VReg a = parseExpr();
        if (failed) return false;
        const Tok cmp = lex.kind;
        if (cmp != Tok::Less && cmp != Tok::LessEq && cmp != Tok::Greater &&
            cmp != Tok::GreaterEq && cmp != Tok::EqEq && cmp != Tok::NotEq) {
            freeTemp(a);
            fail("expected a comparison: <, <=, >, >=, == or !=");
            return false;
        }
        const bool aFixed = exprIsFixed;
        const int  aLit   = exprLitConst;
        lex.advance();
        VReg b = parseExpr();
        if (failed) { freeTemp(a); return false; }
        // Same wall as the operators: agreeing sides compare correctly as raw words (Q16.16
        // preserves order), and disagreeing sides would compare numbers 65,536 apart in meaning.
        // A literal adopts the fixed side here too, which is what makes `if (v < 0)` natural.
        bool cmpFixed = false;
        if (!meet(aFixed, aLit, exprIsFixed, exprLitConst, cmpFixed))
            { freeTemp(b); freeTemp(a); return false; }
        if (!expect(Tok::RParen, "expected ')' to close the if condition")) { freeTemp(b); freeTemp(a); return false; }
        if (!expect(Tok::LBrace, "expected '{': an if body is braced")) { freeTemp(b); freeTemp(a); return false; }

        // Two labels at most: one to skip the then-block, one to skip the else-block.
        if (nextLabel + 2 > kIrLabels) { fail("too many branches in one script"); return false; }
        const uint8_t lElse = nextLabel++;

        // Emit the branch that SKIPS the then-block, which is the NEGATION of the written test.
        switch (cmp) {
            // !(a < b) is a >= b. SIGNED, here and below: this is the script's own comparison,
            // and `a - b` produces a two's-complement negative the moment b exceeds a. Compared
            // unsigned that negative is a huge value and `if (a - b < 0)` never fires.
            case Tok::Less:      emit({IrOp::BranchGeS, 0, a, b, 0,0, lElse, nullptr, {}}); break;
            // !(a >= b) is a < b, which is b > a: BranchGe with the operands swapped tests b >= a,
            // so the strict form needs the pair below. a >= b skips when a < b == b > a.
            case Tok::GreaterEq: emitStrictLess(a, b, lElse); break;
            // !(a > b) is a <= b, i.e. b >= a.
            case Tok::Greater:   emit({IrOp::BranchGeS, 0, b, a, 0,0, lElse, nullptr, {}}); break;
            // !(a <= b) is a > b, i.e. b < a.
            case Tok::LessEq:    emitStrictLess(b, a, lElse); break;
            case Tok::EqEq:      emit({IrOp::BranchNe, 0, a, b, 0,0, lElse, nullptr, {}}); break;
            // !(a != b) is a == b. There is no BranchEq, so this is the one case that needs two:
            // branch over the skip when they differ, then skip unconditionally.
            case Tok::NotEq: {
                if (nextLabel >= kIrLabels) { freeTemp(b); freeTemp(a); fail("too many branches in one script"); return false; }
                const uint8_t lBody = nextLabel++;
                emit({IrOp::BranchNe, 0, a, b, 0,0, lBody, nullptr, {}});
                VReg z = alloc();
                emit({IrOp::Const,    z, 0,0,0,0, 0,     nullptr, {}});
                emit({IrOp::BranchGe, 0, z, z, 0,0, lElse, nullptr, {}});   // z >= z: always taken
                freeTemp(z);
                emit({IrOp::Label,    0, 0,0,0,0, lBody, nullptr, {}});
                break;
            }
            default: break;
        }
        freeTemp(b); freeTemp(a);

        // A braced body is a SCOPE: a local declared inside it dies at the '}', so its name stops
        // resolving and its frame slot is handed back for the next block to reuse. Without this a
        // sixteen-slot frame is spent by the sixteenth `int` anywhere in a function, however
        // short-lived each one was, and a name declared in a then-block would still resolve in the
        // else-block. Saved and restored exactly as parseFor does for its loop counter.
        const uint8_t thenLocal = localCount, thenSlot = slotHighWater;
        while (!failed && lex.kind != Tok::RBrace && lex.kind != Tok::End)
            if (!parseStatement()) return false;
        if (failed) return false;
        localCount = thenLocal; slotHighWater = thenSlot;
        if (!expect(Tok::RBrace, "expected '}' to close the if body")) return false;

        if (atKeyword("else", 4)) {
            lex.advance();
            if (nextLabel >= kIrLabels) { fail("too many branches in one script"); return false; }
            const uint8_t lEnd = nextLabel++;
            // The then-block falls through to here, and must jump OVER the else-block. An
            // unconditional jump is `x >= x`, which the assemblers already encode.
            VReg z = alloc();
            emit({IrOp::Const,    z, 0,0,0,0, 0,    nullptr, {}});
            emit({IrOp::BranchGe, 0, z, z, 0,0, lEnd, nullptr, {}});
            freeTemp(z);
            emit({IrOp::Label,    0, 0,0,0,0, lElse, nullptr, {}});
            if (!expect(Tok::LBrace, "expected '{': an else body is braced")) return false;
            const uint8_t elseLocal = localCount, elseSlot = slotHighWater;
            while (!failed && lex.kind != Tok::RBrace && lex.kind != Tok::End)
                if (!parseStatement()) return false;
            if (failed) return false;
            localCount = elseLocal; slotHighWater = elseSlot;
            if (!expect(Tok::RBrace, "expected '}' to close the else body")) return false;
            emit({IrOp::Label, 0, 0,0,0,0, lEnd, nullptr, {}});
        } else {
            emit({IrOp::Label, 0, 0,0,0,0, lElse, nullptr, {}});
        }
        return true;
    }

    /// Branch to `label` when `a < b`, STRICTLY. BranchGeS gives `>=` only, so the strict form is
    /// "not (a >= b)": branch over an unconditional jump. Two branches for the two comparisons a
    /// single `>=` cannot express, rather than a fourth branch op every backend must grow.
    void emitStrictLess(VReg a, VReg b, uint8_t label) {
        if (nextLabel >= kIrLabels) { fail("too many branches in one script"); return; }
        const uint8_t lSkip = nextLabel++;
        emit({IrOp::BranchGeS, 0, a, b, 0,0, lSkip, nullptr, {}});  // a >= b: do NOT take the skip
        VReg z = alloc();
        emit({IrOp::Const,    z, 0,0,0,0, 0,     nullptr, {}});
        // Unsigned on purpose: `z >= z` is the unconditional-jump IDIOM, not a comparison, and it
        // is true either way. Keeping it on BranchGe leaves the signed op meaning exactly one
        // thing, which is what a reader checking "is this comparison signed?" needs.
        emit({IrOp::BranchGe, 0, z, z, 0,0, label, nullptr, {}});   // always taken
        freeTemp(z);
        emit({IrOp::Label,    0, 0,0,0,0, lSkip, nullptr, {}});
    }

    /// `int x = expr;` inside a function body: a LOCAL, living in a frame slot for the rest of its
    /// block.
    ///
    /// A member was the only place to put a value before this, which was wrong on three counts: a
    /// member is PERSISTED (a transient sensor reading written to config), it is a memory load
    /// rather than a register, and there are only eight member records against sixteen frame slots.
    /// The two are separate budgets, so a local costs a member nothing.
    ///
    /// `int` only, which is what a frame slot holds: a byte or a bool would be the same slot with a
    /// narrower promise, and `fixed` needs the fixed-ness tracked per local, which the slot record
    /// has nowhere to put. Both are worth adding when a script wants them; refusing is honest until
    /// then.
    bool parseLocalDecl() {
        // Every VALUE type a member has: int, byte, bool and fixed all mean the same inside a
        // function as they do in the class body, so a script author meets no arbitrary wall. Only
        // `string` is refused, because there is no runtime string to put in a slot.
        const CtrlType declType = currentType();
        if (declType == CtrlType::Str) {
            fail("a local variable holds a number: string is a member type");
            return false;
        }
        const bool wantFixed = (declType == CtrlType::Fixed);
        lex.advance();
        if (lex.kind != Tok::Ident) { fail("expected a variable name"); return false; }
        const char* varName = lex.identBeg;
        const size_t varLen = lex.identLen;
        // The same three collisions a loop counter checks for, and for the same reasons: a system
        // variable is read-only, a duplicate name binds a second slot to one name, and shadowing a
        // member would make `x = 1` write somewhere the author did not mean.
        if (isReservedWord(varName, varLen)) { fail("that name is a reserved word"); return false; }
        // The same collision a MEMBER is checked for: a local named `fill` would shadow the builtin
        // for the rest of the function, so the call a script wrote next would resolve to a variable.
        if (table.find(varName, varLen)) { fail("that name shadows a built-in function"); return false; }
        if (sysvars.find(varName, varLen)) { fail("name is a system variable"); return false; }
        if (findLocal(varName, varLen) >= 0) { fail("that name is already in use here"); return false; }
        if (findMember(varName, varLen) >= 0) { fail("a member of that name is declared"); return false; }
        if (localCount >= kMaxLocals || slotHighWater >= kMaxLocals) {
            fail("too many variables in this function");
            return false;
        }
        lex.advance();
        if (!expect(Tok::Assign, "a local variable is initialized: int x = 0;")) return false;
        VReg init = parseExpr();
        if (failed) return false;
        // The initializer sets the scaling the declaration promised, and a LITERAL adopts it:
        // `fixed d = 0;` is the natural way to start a fixed at zero, exactly as a fixed member's
        // initializer reads. Anything computed names its own conversion.
        if (wantFixed != exprIsFixed) {
            bool adopted = false;
            if (wantFixed) {
                if (!meet(true, -1, exprIsFixed, exprLitConst, adopted)) return false;
            } else {
                fail("a local variable is a whole number: write toInt(x)");
                return false;
            }
        }
        // A literal is range-checked against the declared type, so `byte b = 300;` names the
        // mistake rather than silently holding 44, exactly as a member's initializer does.
        if (exprLitConst >= 0 && !fitsInType(ir.ops[exprLitConst].imm, declType)) {
            fail("this number is out of range for that type");
            return false;
        }
        const uint8_t slot = slotHighWater++;
        if (slotHighWater > slotsUsed) slotsUsed = slotHighWater;
        narrowToType(init, declType);
        emit({IrOp::Spill, 0, init, 0,0,0, slot, nullptr, {}});
        freeTemp(init);
        locals[localCount++] = {varName, varLen, slot, declType};
        return expect(Tok::Semicolon, "expected ';' after a variable declaration");
    }

    bool parseStatement() {
        if (atTypeKeyword()) return parseLocalDecl();
        if (atKeyword("for", 3)) return parseFor();
        if (atKeyword("if", 2))  return parseIf();
        if (atKeyword("return", 6)) return parseReturn();
        if (lex.kind != Tok::Ident) { fail("expected a function call or an assignment"); return false; }
        // One token of lookahead separates `name = …` from `name(…)`. Both start with an
        // identifier, and only the token AFTER it says which, so the name is saved and the lexer
        // rewound rather than committing to either shape.
        const char* name = lex.identBeg;
        const size_t nameLen = lex.identLen;
        Lexer save = lex;
        lex.advance();
        // `name =` and `name[` both start an assignment; `name(` is a call.
        if (lex.kind == Tok::Assign || lex.kind == Tok::LBracket) return parseAssignment(name, nameLen);
        lex = save;
        parseCall(nullptr);
        if (failed) return false;
        return expect(Tok::Semicolon, "expected ';'");
    }

    /// returnStmt := "return" [expr] ";"
    ///
    /// Two jobs. Inside `tick()` it is an EARLY EXIT, which is what a script wants when a guard
    /// fails and the rest of the frame has nothing to do. And it is how a function ANSWERS, which
    /// is what `dimensions()` and `tags()` are: the host calls them and reads the value.
    ///
    /// The value is a machine word, like every other value here: a number, or the pointer a string
    /// literal already compiles to. Nothing checks that a function returns consistently, in keeping
    /// with the rest of the language, and a function that never returns one answers 0.
    bool parseReturn() {
        lex.advance();                       // past `return`
        if (lex.kind == Tok::Semicolon) {    // a bare return: unwind, no value
            // A bare return in a function that promised a value would leave the caller reading
            // whatever sat in the return register, which is the exact failure the declaration
            // exists to prevent.
            if (curRet != RetType::Void) { fail("this function must return a value"); return false; }
            emit({IrOp::Ret, 0, 0,0,0,0, 0, nullptr, {}});
            lex.advance();
            return true;
        }
        // A value from a `void` function has nowhere to go: the host calls it for its effect and
        // never looks at the register.
        if (curRet == RetType::Void) { fail("a void function returns no value"); return false; }
        VReg v = 0;
        if (lex.kind == Tok::String) {
            if (curRet != RetType::Str) { fail("this function returns an int, not a string"); return false; }
            // A string is a POINTER, which is a value like any other here. Returning one is how
            // tags() answers, and it is the only way a script hands text to the host: an expression
            // cannot otherwise carry a string, and does not need to.
            //
            // Interned rather than pointed at the source, for the same reason a control label is:
            // the text must outlive the compile, and an interned copy is final the moment it is
            // made.
            const char* interned = internString(lex.identBeg, lex.identLen);
            if (!interned) { fail("no room for this script's strings"); return false; }
            v = alloc();
            emit({IrOp::ConstPtr, v, 0,0,0,0, 0, nullptr, interned, {}});
            lex.advance();
        } else {
            if (curRet != RetType::Int) { fail("this function returns a string, not a number"); return false; }
            v = parseExpr();
            if (failed) return false;
        }
        // imm 1 says "this return carries a value", which is what the spill pass reads to decide
        // whether `a` is a register it must keep alive.
        emit({IrOp::Ret, 0, v, 0,0,0, 1, nullptr, {}});
        freeTemp(v);
        return expect(Tok::Semicolon, "expected ';'");
    }

    /// The body of one named function: `name() { statements }`, with the name already consumed.
    /// Stage 1 emits it INLINE at the point the class body reaches it, which is what makes `tick()`
    /// the whole program while it is the only entry point. Real per-function frames arrive with the
    /// call support in this same step; this is the parse shape they will attach to.
    /// The parameter list, parsed BEFORE the function emits anything: the count decides how many
    /// copy-out instructions its prologue needs, and inserting into the IR afterwards would shift
    /// every index the spill pass and the call lowering rely on.
    bool parseParams(uint8_t* paramsOut) {
        if (!expect(Tok::LParen,  "expected '(' after the function name")) return false;
        // The locals must be cleared BEFORE the parameters are declared: they become locals 0..N-1
        // of this function's frame, and a leftover from the previous function would push them up
        // and leave the caller staging into the wrong slots.
        localCount = 0; slotHighWater = 0;
        uint8_t params = 0;
        while (lex.kind != Tok::RParen && lex.kind != Tok::End) {
            if (params > 0 && !expect(Tok::Comma, "expected ',' between parameters")) return false;
            // A parameter is declared like a variable, and its type is read the same way, so
            // `int`/`byte`/`bool` all mean here what they mean anywhere else.
            if (!atTypeKeyword()) { fail("expected a parameter type"); return false; }
            const CtrlType t = currentType();
            lex.advance();
            if (lex.kind != Tok::Ident) { fail("expected a parameter name"); return false; }
            if (localCount >= kMaxLocals) { fail("too many locals"); return false; }
            if (params >= kMaxCallArgs) { fail("too many parameters"); return false; }
            locals[localCount++] = {lex.identBeg, lex.identLen, slotHighWater, t};
            slotHighWater++;
            if (slotHighWater > slotsUsed) slotsUsed = slotHighWater;
            params++;
            lex.advance();
        }
        if (!expect(Tok::RParen,  "expected ')' to close the parameter list")) return false;
        if (paramsOut) *paramsOut = params;
        return true;
    }

    /// The body. Its parameters are already locals 0..N-1, declared by parseParams.
    bool parseFunctionBody() {
        if (!expect(Tok::LBrace,  "expected '{' to open the function body")) return false;
        // Cleared above, BEFORE the parameters were declared, so they are in scope for the body:
        // clearing here would discard them. Each function still starts with an empty scope, which
        // is what stops one function's locals stacking on the next's.
        while (!failed && lex.kind != Tok::RBrace && lex.kind != Tok::End)
            if (!parseStatement()) return false;
        if (failed) return false;
        localCount = 0; slotHighWater = 0;
        return expect(Tok::RBrace, "expected '}' to close the function body");
    }

    /// program := "class" NAME "{" { decl | function } "}"
    ///
    /// ONE top-level form. A bare statement list is no longer accepted: keeping it would mean two
    /// parse paths, two sets of rules to document and two things to test, permanently, so that the
    /// shortest scripts could stay one line shorter. The class declaration is what makes a script
    /// read as the module it stands in for, which is the whole point of the shape.
    bool parseProgram() {
        if (!atKeyword("class", 5)) { fail("a script is a class: expected `class <Name> { … }`"); return false; }
        lex.advance();
        if (lex.kind != Tok::Ident) { fail("expected a name after `class`"); return false; }
        // Copied, not borrowed: the source buffer is freed as soon as the compile returns, and the
        // name outlives it in the status line.
        const size_t n = lex.identLen < kMaxClassName ? lex.identLen : kMaxClassName;
        std::memcpy(classNameOut, lex.identBeg, n);
        classNameOut[n] = '\0';
        lex.advance();
        if (!expect(Tok::LBrace, "expected '{' to open the class body")) return false;

        // Declarations first (the controls), then the functions. Both live inside the braces now.
        //
        // A member and a typed function open with the SAME token: `int speed = 50;` and
        // `int dimensions() { … }`. Two tokens of lookahead separate them, and only the one after
        // the name says which, so the lexer is saved and rewound rather than committing. Without
        // this the member parser swallows `int dimensions` and then fails on the '(' it did not
        // expect, reporting a member error for a perfectly good function.
        while (!failed && atTypeKeyword()) {
            const CtrlType ty = currentType();
            Lexer save = lex;
            lex.advance();                       // past the type
            const bool isFn = lex.kind == Tok::Ident && [&] {
                Lexer probe = lex;
                probe.advance();                 // past the name
                return probe.kind == Tok::LParen;
            }();
            if (isFn) { lex = save; break; }     // a function: leave it for the loop below
            parseDecl(ty);
        }
        if (failed) return false;

        bool any = false;
        while (!failed && lex.kind != Tok::RBrace && lex.kind != Tok::End) {
            // Every function DECLARES what it hands back, so a script reads like the compiled module
            // it stands in for (`void tick()` beside `void tick() override`) and the host can tell a
            // function that answers from one that acts. Not optional: a bare name accepted as
            // implicit void would leave the two spellings meaning the same thing forever, and the
            // point of the declaration is that the host can rely on it.
            if (!atRetKeyword()) {
                fail("a function declares what it returns: void, int or string");
                return false;
            }
            const RetType ret = currentRet();
            lex.advance();                       // past the return type
            if (lex.kind != Tok::Ident) { fail("expected a function name after its return type"); return false; }
            if (fnCount >= kMaxEntryPoints) { fail("too many functions in one class"); return false; }
            // The engine copies entry names into a fixed buffer, so a longer one would be
            // TRUNCATED there. Two functions sharing a 23-character prefix would then land under
            // the same name and `entry()` would return whichever came first: a call dispatched to
            // the wrong function, silently. Refused here, where a control name already is, so the
            // script author is told rather than the engine guessing.
            if (lex.identLen > kMaxEntryName) { fail("function name too long"); return false; }
            // The NAME is captured before the SIGNATURE is parsed, since parsing moves the lexer,
            // and the signature is parsed before anything is emitted: the parameter count decides
            // how many copy-out instructions the prologue needs, and inserting into the IR after
            // the fact would shift every index the spill pass and the call lowering rely on.
            const char* const fnName = lex.identBeg;
            const uint8_t fnNameLen = static_cast<uint8_t>(lex.identLen);
            lex.advance();                       // the function name
            uint8_t params = 0;
            if (!parseParams(&params)) return false;
            fns[fnCount] = {fnName, fnNameLen, static_cast<uint16_t>(ir.count), ret, params};
            curRet = ret;                        // every `return` below is checked against it
            // The IR carries the start INDEX; the lowering turns it into a byte offset.
            ir.fnIrStart[fnCount] = static_cast<uint16_t>(ir.count);
            ir.fnCount = static_cast<uint8_t>(fnCount + 1);
            fnCount++;
            // Park the host arguments in this FUNCTION's frame. Read-only, so one store each, and
            // every later read is a Reload, which frees five registers for the body. Per function
            // rather than per program because each function owns its own frame now: a spill emitted
            // before the first prologue would write to a frame that does not exist yet.
            for (VReg v = 0; v < kFirstTemp; v++)
                emit({IrOp::Spill, 0, v, 0,0,0, hostArgSlot(v), nullptr, {}});
            // COPY THE ARGUMENTS OUT of the shared arena block into this function's own slots,
            // before the body runs and so before it can make a call of its own. That copy is what
            // lets ONE arena block serve every depth: see kScriptArgBase. It also has to happen
            // here rather than lazily at each use, since a call in the middle of the body would
            // overwrite the block the later uses would read.
            for (uint8_t a = 0; a < params; a++) {
                const VReg v = alloc();
                emit({IrOp::LoadCtrl32, v, 0,0,0,0, scriptArgOffset(a), nullptr, {}});
                emit({IrOp::Spill, 0, v, 0,0,0, a, nullptr, {}});
                freeTemp(v);
            }
            if (!parseFunctionBody()) return false;
            any = true;
        }
        if (failed) return false;
        if (!any) { fail("a class with no function does nothing"); return false; }
        return expect(Tok::RBrace, "expected '}' to close the class");
    }
};

}  // namespace

uint32_t countTokens(const char* source) {
    if (!source) return 0;
    uint32_t tokens = 0;
    for (Lexer scan(source); scan.kind != Tok::End && scan.kind != Tok::Error; scan.advance()) {
        if (++tokens > kMaxIrOps) break;   // runaway source — reserve() rejects past the bound
    }
    return tokens;
}

CompileResult compileSource(const char* source, const BuiltinTable& table,
                            const SysVarTable& sysvars, uint8_t* out, size_t cap,
                            const RegBudget* squeeze, LowerFn lower,
                            char* strings, uint16_t stringCap) {
    CompileResult r;
    if (!source) { r.error = "no source"; return r; }
    if (!out || cap == 0) { r.error = "no code buffer"; return r; }

    // Size the op array to THIS script before parsing. The bound is per-TOKEN rather than
    // per-construct: no token the lexer can produce lowers to more than a handful of ops (the
    // densest is a call argument — evaluate, then the Call itself), so counting tokens and
    // multiplying is an over-estimate that cannot undershoot. Over-estimating costs a few unused
    // entries on a cold path; undershooting would fail a script that fits, so the direction of the
    // error is the whole point. push() still refuses past `cap`, so a wrong estimate degrades with
    // a diagnostic rather than corrupting memory.
    const uint32_t tokens = countTokens(source);
    IrProgram ir;
    // +8 covers a program's fixed overhead (the prologue/epilogue ops a tiny script still needs)
    // so a one-token source cannot round down to nothing.
    if (!ir.reserve(static_cast<uint16_t>(tokens * kIrOpsPerToken + 8 > kMaxIrOps
                                          ? kMaxIrOps : tokens * kIrOpsPerToken + 8))) {
        r.error = "script too large";
        return r;
    }
    Lexer lex(source);
    Parser parser{lex, table, sysvars, ir, r.className};
    parser.strings = strings;      // where string literals are interned; see compileSource
    parser.stringCap = stringCap;
    if (!parser.parseProgram()) { r.error = parser.error; r.errorCol = parser.errorCol; return r; }
    // Hand the backend the frame the script's variables need. The register allocator numbers any
    // further slot from here up, so the two never overlap in the one frame they share.
    ir.localSlots = parser.slotsUsed;

    // `lower` is normally this build's own backend; a test passes a DIFFERENT ISA's lowerer to read
    // what a device would execute without flashing one. The front end is identical either way, which
    // is the point — the seam is one function pointer, not a second copy of the compiler.
    size_t len = lower ? lower(ir, out, cap, squeeze) : lowerToBytes(ir, out, cap, squeeze);
    if (len == 0) { r.error = kCodegenFailed; return r; }
    r.ok = true;
    r.len = len;
    // Surface the declared controls so the binding can create real MoonModule controls.
    r.stringLen = parser.stringLen;
    r.memberCount = parser.memberCount;
    for (uint8_t i = 0; i < parser.memberCount; i++) r.members[i] = parser.members[i];
    // The functions the class defined, each with the byte its code starts at. The parser recorded
    // an IR index and the lowering converted it while emitting, so this is a real symbol table: a
    // binding asks for an entry by name and gets an address inside the one emitted block.
    r.entryCount = parser.fnCount;
    for (uint8_t i = 0; i < parser.fnCount; i++)
        // The declared return type travels with the entry all the way to the engine: dropped
        // here, every function reads as Void and runValue refuses to answer for any of them.
        r.entries[i] = {parser.fns[i].name, parser.fns[i].nameLen, ir.fnOffset[i],
                        parser.fns[i].ret};
    return r;
}

}  // namespace mm::moonlive
