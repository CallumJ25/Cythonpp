#include "literal_guard.h"

#include <string>

#include "domain/ast/constant.h"
#include "domain/ast/unary_op.h"
#include "domain/lexer/token_type.h"

namespace cythonpp::domain::semantic {
namespace {

// The LITERAL_INT half of literal_guard_verdict, split out because the lexeme
// test is the one place in this file where the cheap implementation is wrong in
// the UNACCEPTABLE direction and so deserves to be read on its own.
//
// A WHITELIST, never a blacklist: fold only when every character is a decimal
// digit or `_`. The obvious alternative -- "is the lexeme all zeros" -- folds
// `0x0` TRUE, because `0x0` is not all zeros. mypy folds it FALSE (measured
// 2026-09-17: `while c:` / `if 0x0: return 1` / `break` / `else: return 3` is
// `Missing return statement [return]`, because the dead body leaves the `break`
// live), so folding it TRUE would kill that break, guarantee the `else`, and
// make cythonpp SILENT on a program mypy rejects. Verified via `--tokens` that
// `0x0` really does arrive here as a LITERAL_INT whose lexeme is "0x0", and
// that `0X0` arrives as "0X0" -- which is why this is a per-CHARACTER test and
// not a check for the prefix `0x`: the upper-case spellings `0X`/`0O`/`0B` are
// as much non-decimal as the lower-case ones (all six measured to fold FALSE),
// and a prefix check written for one case admits the other.
//
// A LEADING ZERO followed by any nonzero digit is NOT a legal Python decimal
// literal at all (`0_1`, `01`), and this returns Unknown for it rather than
// folding it TRUE. Measured 2026-09-17: `if 0_1:` is a mypy BLOCKING
// `Leading zeros in decimal integer literals are not permitted [syntax]` and a
// CPython `SyntaxError`, so BOTH oracles reject the program; cythonpp has no
// diagnostic of its own for it (a separate, pre-existing parser gap) and today
// only exits non-zero because of the very `missing return statement` this fix
// removes. Folding it TRUE would therefore turn a right-verdict/wrong-message
// rejection into silent acceptance. `00`/`000`/`0_0` are legal and all-zero, so
// they fold FALSE and are unaffected by this clause.
GuardVerdict decimal_int_guard_verdict(const std::string& lexeme) {
    char first_digit = '\0';
    bool all_zero = true;
    bool previous_was_underscore = false;
    for (const char character : lexeme) {
        if (character == '_') {
            // UNDERSCORE PLACEMENT IS VALIDATED, NOT SKIPPED, and this is the
            // same class of defect as the leading-zero clause below rather
            // than a tidiness rule. Python's grammar puts a single underscore
            // strictly BETWEEN digits, so a LEADING one, a TRAILING one, or a
            // DOUBLED run is not an integer literal at all -- but the lexer
            // still hands it over as LITERAL_INT (verified via --tokens:
            // `1_`, `1__0` and `0_` all arrive here). Measured 2026-09-17,
            // `if 1_:` is a mypy blocking `Invalid decimal literal  [syntax]`
            // AND a CPython `SyntaxError: invalid decimal literal` at exit 1,
            // so BOTH oracles reject the program. Skipping every `_`
            // unconditionally folded `1_`/`1__0`/`1_2_`/`12__3` TRUE and
            // `0_`/`0__0` FALSE, which REMOVED the missing-return that was
            // cythonpp's only diagnostic on those programs -- silent
            // acceptance of a doubly-rejected program, at all five wired
            // sites including check_suite's suppression, and it reached
            // codegen as a third state (the emitter strips the underscore, so
            // `1_` emitted as C++ `py::int_(1)`, compiled at exit 0 and
            // printed `1` where CPython prints nothing and exits 1). Found by
            // adversarial review of the commit that introduced it.
            if (first_digit == '\0' || previous_was_underscore) {
                return GuardVerdict::Unknown;
            }
            previous_was_underscore = true;
            continue;
        }
        if (character < '0' || character > '9') {
            return GuardVerdict::Unknown;
        }
        previous_was_underscore = false;
        if (first_digit == '\0') {
            first_digit = character;
        }
        if (character != '0') {
            all_zero = false;
        }
    }
    if (previous_was_underscore) {
        // A trailing underscore -- the other half of the placement rule
        // above, and not reachable from the in-loop check.
        return GuardVerdict::Unknown;
    }
    if (first_digit == '\0') {
        // No digits at all -- not a spelling this function can read.
        return GuardVerdict::Unknown;
    }
    if (all_zero) {
        return GuardVerdict::AlwaysFalse;
    }
    if (first_digit == '0') {
        return GuardVerdict::Unknown;
    }
    return GuardVerdict::AlwaysTrue;
}

} // namespace

// LITERAL CONDITION FOLDING, 2026-09-17. mypy prunes a statically-decided
// branch before asking any reachability question; this compiler's reachability
// helpers had no constant folding at all, which produced false positives in
// four separate places on programs both oracles accept and run -- the most
// ordinary of them needing no loop, no `break` and no dead code:
// `def f() -> int:` / `if True: return 1` was a false
// `missing return statement`.
//
// THE AUTHORITY IS MYPY'S OWN SOURCE, not a guess at its intent: the two
// helpers at the top of `find_isinstance_check_helper`, mypy 1.18.1
// `checker.py:8255`, are
//
//     def is_true_literal(n):  refers_to_fullname(n, "builtins.True")
//                              or isinstance(n, IntExpr) and n.value != 0
//     def is_false_literal(n): refers_to_fullname(n, "builtins.False")
//                              or isinstance(n, IntExpr) and n.value == 0
//
// so an int literal folds BY VALUE (re-measured directly: `0x1`, `0b1`, `0o1`,
// `1_0` and `(1)` all fold TRUE), and mypy's own prune set is wider than what
// is implemented below.
//
// THE GATE IS THE AST SHAPE, NEVER THE TYPE -- a bare `ast::Constant` and
// nothing else. This is the same discipline `emit_power` uses for `**`, adopted
// for the same recorded reason: a NEGATIVE literal parses as
// `UnaryOp(-, Constant)`, so the sign lives in a node the TYPE cannot see, and
// `-1` types as `int` exactly as `1` does. Requiring a bare `Constant` excludes
// `-1`, `+1` and `-0` for free, with no unary logic to get wrong -- and it is
// mypy's own exclusion, structurally: `-1` is a `UnaryExpr`, never an
// `IntExpr`, so it never reaches the test above, which is exactly why mypy
// folds `(1)` but not `(-1)`. A type-based gate would be wrong in the
// DANGEROUS direction. All three measured 2026-09-17: `if -1:`, `if +1:` and
// `if -0:` guarding a return leave mypy reporting `Missing return statement`.
//
// TWO mypy-UNSOUND FORMS ARE EXCLUDED BY CONSTRUCTION, and a future widening
// must not admit them. Measured 2026-09-17: mypy prunes `NotImplemented` as
// always-true while CPython raises
// `TypeError: NotImplemented should not be used in a boolean context` (exit 1),
// and mypy prunes `not TYPE_CHECKING` while the pruned branch actually RUNS,
// returning None from an `-> int` function. Following mypy on either would
// compile a program CPython refuses to run. Both are bare `ast::Name`s, never
// an `ast::Constant`, so neither can reach this function at all.
//
// THAT LAST SENTENCE IS ONLY TRUE WHILE `and`/`or` STAY OUT, and a future
// widening must not read it as unconditional. Measured 2026-09-20: under the
// Kleene rule below, `NotImplemented and False` folds FALSE from the DECIDING
// SIBLING alone -- the bare Name never needs a verdict of its own, so
// "excluded by construction" stops holding. It is harmless today only by
// accident, because a bare `NotImplemented` draws a (NotSuppressible, and
// itself wrong) `NameError` here, so the program still exits 1; close that
// gap and the fold goes silent on a CPython-rejected program. Note this is
// NOT the same as the `ZeroDivisionError` family, where cythonpp's own
// runtime reproduces the failure faithfully -- there is no `NotImplemented`
// in `runtime/` to reproduce anything with.
//
// AND THE CODEGEN HALF WOULD CLOSE AT THE SAME MOMENT, which is why this is
// a warning and not a curiosity. Measured 2026-09-20: `--emit-cpp` on
// `if NotImplemented and False:` refuses with no file written -- but the
// refusal IS that same `NameError`, not a guard of the emitter's own. So the
// reassuring-sounding claim "the emitter would refuse it anyway" is NOT
// established; closing the `NameError` gap removes both defences together.
// Whether anything downstream would then refuse it is UNDETERMINED and must
// be measured, not assumed, by whoever closes that gap.
//
// DELIBERATE OMISSIONS, each measured-foldable under mypy and each left out:
// `...` (ELLIPSIS), tuple displays (which fold by LENGTH -- `(0,)` folds TRUE),
// `and`/`or`, and non-decimal int literals. For every one EXCEPT `and`/`or`,
// omitting the form only ever RETAINS a false positive -- the safe direction
// -- and adding it later is purely additive.
//
// `and`/`or` IS THE EXCEPTION, which is why it needs the paragraph below
// rather than a line in this list: adding it is NOT purely additive, because
// a deciding sibling can decide a condition whose other operand is a lexeme
// BOTH ORACLES REJECT, removing a diagnostic rather than adding one. See
// reason (2).
//
// `and`/`or` ARE EXCLUDED BY DECISION, NOT BY DIFFICULTY -- and the reason
// recorded here until 2026-09-20 was WRONG, so do not restore it. It said
// mypy's `and` is ONE-sided while its `or` is TWO-sided, so neither "is
// expressible as a fold over operand verdicts the way `not` is". Every
// premise is true and the conclusion is false: a fold over operand verdicts
// expresses mypy's behaviour everywhere it was measured, and that asymmetry
// IS the ordinary duality of three-valued logic (`and` short-circuits on
// FALSE so one FALSE operand decides it; `or` short-circuits on TRUE so one
// TRUE operand decides IT). Measured 2026-09-20: 52 verdict cells -- the 3x3
// matrix for each operator, composition with every existing exclusion, n-ary
// chains, mixed precedence, and name/call operands -- all Kleene, ZERO
// mismatches. Every existing exclusion composes for free, `-1 and False`
// folding FALSE because `False` decides what the signed literal cannot.
//
// BUT IT IS NOT AN EQUIVALENCE, and do not write that it is. Found by
// adversarial review, re-derived here: mypy's `and`/`or` reachability is
// TYPE-based (`can_be_false` over the narrowed type), not a fold over
// operand verdicts, so it can be MORE decided than Kleene. Measured,
// `if -1 and 1:` is `Missing return statement` -- UNDECIDED, both operands
// being Unknown to mypy itself -- while `if (-1 and 1) or (-1 and 1):` is
// `Success`, i.e. TRUE, where Kleene gives `Unknown or Unknown = Unknown`.
// It needs int LITERAL types: the `0.0` and `""` spellings of the same shape
// stay undecided. The divergence is always in the SAFE direction (a targeted
// 196-case sweep found no case where Kleene decides and mypy does not), so a
// Kleene fold would still be a strict subset -- but the subset property is
// an EMPIRICAL result, not the equivalence it was first written as. The real reasons to decline are in
// `.claude/specs/2026-09-20-boolop-condition-folding-design.md` and are about
// VALUE, not expressibility:
//   (1) the motivating idiom does not fold. A named `bool` debug flag is
//       mypy-Unknown -- `DEBUG = True` / `if c or DEBUG:` is `Missing return
//       statement`, and so is the `Final` spelling; only `Literal[True]`
//       folds, and that needs an import the parser refuses. So only an inline
//       bare literal folds, and for those the plain `if True:` / `if False:`
//       spelling already folds today.
//   (2) a Kleene fold would reverse the semantic half of `4c62de1` by a side
//       route: `if 1_ or True: return 1` folds TRUE from the sibling, so the
//       malformed lexeme never needs the verdict that commit deliberately
//       withheld, and cythonpp's only diagnostic on a program BOTH oracles
//       reject as a SyntaxError disappears. Five measured shapes. Guarding it
//       needs poison-propagation through `not` and nested `BoolOp`s, which
//       re-litigates `3d672c2`'s decision not to model Python's numeric
//       grammar.
//
// EXCLUDED BECAUSE MYPY DOES NOT FOLD THEM, and including any would make
// cythonpp accept a program mypy rejects: every signed number, float, complex,
// str, bytes, list, dict and set. Note in particular that the falsy prune set
// recorded elsewhere in this project as `{False, 0, None}` is INCOMPLETE rather
// than wrong -- re-measured 2026-09-17, `()` is also pruned and `0.0` is NOT,
// so the set is neither "numeric zero" nor "any empty container".
GuardVerdict literal_guard_verdict(const ast::Expr& condition) {
    // `not` INVERTS a decided operand and leaves an undecided one undecided,
    // which is exactly mypy's behaviour and -- because it RECURSES -- gets the
    // nesting and the exclusions for free rather than by enumeration.
    // Measured 2026-09-17/18 in both directions: `not False`, `not 0`,
    // `not None` and `not not True` fold TRUE; `not True`, `not 1` and
    // `not not False` fold FALSE; and `not ""`, `not 0.0`, `not []`, `not -1`
    // and `not 0x1` fold NEITHER -- the last five fall out with no special
    // case at all, since their operands are Unknown and Unknown inverts to
    // Unknown. Seven false positives closed, every one mypy-Success and
    // CPython-clean.
    //
    // `and`/`or` are still excluded, but NOT because they cannot be written
    // this way -- they can, and the claim that they cannot was refuted by
    // measurement on 2026-09-20. A Kleene fold over operand verdicts matches
    // mypy everywhere it was measured, so the arm would be a sibling of this
    // one: `and` yields AlwaysFalse if ANY operand is AlwaysFalse and
    // AlwaysTrue if ALL are AlwaysTrue; `or` is the dual. (It is a SUBSET of
    // mypy, not an equivalence -- see the header block, which records the
    // measured case where mypy is more decided than Kleene.) See that block
    // for the two measured reasons it was declined anyway, both about VALUE
    // rather than expressibility.
    //
    // A reader who implements it should know two shapes that are easy to get
    // wrong. The loop must NOT early-exit on the first undecided operand --
    // `"" and False` folds FALSE from the SECOND one. And `ast::BoolOp` is
    // n-ary for an unparenthesised run (`a and b and c` is ONE node with
    // three values) while a parenthesised same-operator run STAYS NESTED
    // (`a and (b and c)` is a BoolOp inside a BoolOp), so a recursion written
    // for only one of the two mis-folds the other.
    if (const auto* unary = dynamic_cast<const ast::UnaryOp*>(&condition)) {
        if (unary->op() == lexer::token_type::OP_NOT) {
            switch (literal_guard_verdict(unary->operand())) {
            case GuardVerdict::AlwaysTrue:
                return GuardVerdict::AlwaysFalse;
            case GuardVerdict::AlwaysFalse:
                return GuardVerdict::AlwaysTrue;
            case GuardVerdict::Unknown:
                return GuardVerdict::Unknown;
            }
        }
        // Any OTHER unary operator -- `-`, `+`, `~` -- is deliberately NOT
        // looked through: measured, mypy folds neither `-1` nor `+1` nor `-0`,
        // because the sign makes it a UnaryExpr and never an IntExpr. Falling
        // through to the Constant cast below returns Unknown for them, which
        // is the whole reason the gate is the AST SHAPE rather than the type.
        return GuardVerdict::Unknown;
    }
    const auto* constant = dynamic_cast<const ast::Constant*>(&condition);
    if (constant == nullptr) {
        return GuardVerdict::Unknown;
    }
    switch (constant->type()) {
    case lexer::token_type::BOOL_TRUE:
        return GuardVerdict::AlwaysTrue;
    case lexer::token_type::BOOL_FALSE:
    case lexer::token_type::KEYWORD_NONE:
        return GuardVerdict::AlwaysFalse;
    case lexer::token_type::LITERAL_INT:
        return decimal_int_guard_verdict(constant->lexeme());
    default:
        // A default is right here, the same judgement literal_type() makes for
        // its own switch over the same enum: token_type has well over a
        // hundred enumerators and all but these four are either not literals
        // at all or measured NOT to fold. It is NOT the exhaustive-switch
        // idiom `-Werror=switch` guards elsewhere in this codebase (TypeKind,
        // DiagnosticKind, RuleResult::Status), where a missing case must be a
        // compile error.
        return GuardVerdict::Unknown;
    }
}

} // namespace cythonpp::domain::semantic
