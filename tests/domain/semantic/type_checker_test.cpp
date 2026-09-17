#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "domain/ast/module.h"
#include "domain/diagnostics/diagnostic_sink.h"
#include "domain/lexer/indentation_pass.h"
#include "domain/lexer/lexer.h"
#include "domain/parser/statement_parser.h"
#include "domain/semantic/type_checker.h"
#include "domain/semantic/type_map.h"

namespace cythonpp::domain::semantic {
namespace {

struct Checked {
    std::vector<diagnostics::Diagnostic> diagnostics;
    std::size_t typed_expressions = 0;
};

// A whole module through the REAL chain: source -> Lexer -> IndentationPass
// -> StatementParser -> TypeChecker. Hand-building the Module would let these
// tests agree with a wrong belief about what the parser produces.
Checked check_module(const std::string& source) {
    lexer::Lexer lexer(source);
    const lexer::TokenStream lexed(lexer.tokenize());
    diagnostics::DiagnosticSink parse_sink;
    lexer::TokenStream tokens = lexer::IndentationPass().run(lexed, parse_sink);
    const std::unique_ptr<ast::Module> module =
        parser::StatementParser(tokens, parse_sink).parse_module();
    EXPECT_TRUE(parse_sink.empty()) << "fixture must parse cleanly";

    diagnostics::DiagnosticSink sink;
    const TypeMap types = TypeChecker(sink).check(*module);

    Checked result;
    result.diagnostics = sink.diagnostics();
    result.typed_expressions = types.size();
    return result;
}

void expect_clean(const std::string& source) {
    const Checked checked = check_module(source);
    EXPECT_TRUE(checked.diagnostics.empty())
        << "expected clean, got "
        << (checked.diagnostics.empty() ? "" : checked.diagnostics.front().message);
}

diagnostics::Diagnostic only_error(const Checked& checked) {
    EXPECT_EQ(checked.diagnostics.size(), 1u) << "expected exactly one diagnostic";
    if (checked.diagnostics.size() != 1) {
        return diagnostics::Diagnostic{diagnostics::Severity::Error, "", "", 0, 0};
    }
    return checked.diagnostics.front();
}

TEST(TypeChecker, InfersALocalVariablesType) {
    expect_clean("y = 5\nz: int = y\n");
}

TEST(TypeChecker, ChecksAnAnnotatedAssignment) {
    expect_clean("x: int = 5\n");
    expect_clean("x: float = 5\n");           // numeric tower
    expect_clean("x: list[int] = []\n");      // bidirectional checking
    expect_clean("x: int | None = None\n");
}

TEST(TypeChecker, ReportsAnIncompatibleAnnotatedAssignment) {
    const Checked checked = check_module("x: int = \"s\"\n");

    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message,
              "incompatible types in assignment (expression has type \"str\", "
              "variable has type \"int\")");
    EXPECT_EQ(error.line, 1);
}

// Verified: the FIRST assignment's inferred type is sticky.
TEST(TypeChecker, TheFirstAssignmentsInferredTypeIsSticky) {
    const Checked checked = check_module("x = 5\nx = \"s\"\n");

    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.line, 2);
}

TEST(TypeChecker, ReassigningACompatibleValueIsClean) {
    expect_clean("x = 5\nx = 6\n");
    expect_clean("x: float = 1.0\nx = 2\n");
}

// Verified: re-annotating is no-redef, and mypy reports ONLY that -- no
// assignment error alongside it. Asserting the count is what pins that.
TEST(TypeChecker, ReAnnotatingIsARedefinitionAndReportsOnce) {
    const Checked checked = check_module("x: int = 1\nx: str = \"s\"\n");

    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "name \"x\" already defined on line 1");
}

// Pin the MESSAGE, not just the code. The entire
// point of scan_top_level_names's true-source-order pre-pass is that the
// reported line is the genuine FIRST occurrence (line 1, the def) rather
// than whichever phase happens to run first (Phase 1 always declares
// classes before Phase 2 binds functions, so a naive phase-order collision
// check would say "line 3", the class, even though the def is textually
// first). Asserting only `.code` would stay green if that pre-pass were
// reverted to a naive phase-order comparison.
TEST(TypeChecker, ADefAndAClassSharingANameIsARedefinition) {
    const Checked checked = check_module("def n() -> None:\n    pass\nclass n:\n    pass\n");

    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "name \"n\" already defined on line 1");
}

// Same collision, reverse order: the class is textually first this time, so
// the reported line must follow it, not flip back to "always the class" or
// "always the def".
TEST(TypeChecker, AClassAndADefSharingANameIsARedefinitionInReverseOrder) {
    const Checked checked = check_module("class n:\n    pass\ndef n() -> None:\n    pass\n");

    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "name \"n\" already defined on line 1");
}

// An AnnAssign followed by a colliding `def` was
// silently missed -- collect_signatures called ScopeStack::bind for the def
// and discarded the returned bool, so no diagnostic fired AND the def's
// signature binding was silently dropped (a later call would have been
// typed against the AnnAssign's `int`, not the function's signature).
// Verified against mypy 1.18.1: "error: Name "x" already defined on line 1
// [no-redef]".
TEST(TypeChecker, AnAnnAssignFollowedByACollidingDefIsARedefinition) {
    const Checked checked = check_module("x: int = 1\ndef x() -> None:\n    pass\n");

    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "name \"x\" already defined on line 1");
}

// The reverse order already worked before this fix round, since
// bind_annotation (unlike the def arm) already checked ScopeStack::bind's
// return value -- pinned here so a future change cannot silently flip it.
TEST(TypeChecker, ADefFollowedByACollidingAnnAssignIsARedefinition) {
    const Checked checked = check_module("def x() -> None:\n    pass\nx: int = 1\n");

    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "name \"x\" already defined on line 1");
}

// A pure def/def collision was already caught pre-fix, via
// scan_top_level_names (which compares EVERY top-level ClassDef/FunctionDef
// name against every other, not just class-vs-def) -- unlike the
// AnnAssign-then-def case above, it never reached the discarded-bool code in
// collect_signatures at all, since scan_top_level_names's own
// collided_top_level_ set makes collect_signatures skip the second def
// entirely. Added here as adjacent regression coverage, not as a case this
// fix round newly repairs. Verified against mypy 1.18.1.
TEST(TypeChecker, ADefFollowedByACollidingDefIsARedefinition) {
    const Checked checked =
        check_module("def n() -> None:\n    pass\ndef n() -> None:\n    pass\n");

    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "name \"n\" already defined on line 1");
}

// Verified: bare x = [] is var-annotated.
TEST(TypeChecker, ReportsABareEmptyContainer) {
    const Checked checked = check_module("x = []\n");

    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "need type annotation for \"x\"");
}

// Verified: x = () is CLEAN -- tuple[()] is a complete non-generic type.
TEST(TypeChecker, ABareEmptyTupleIsClean) {
    expect_clean("x = ()\n");
}

// ReportsABareEmptyContainer above only pins `[]`.
// `{}` and the five zero-argument constructor calls are implemented by the
// SAME is_bare_empty_container check but had no coverage of their own.
TEST(TypeChecker, ReportsABareEmptyDictDisplay) {
    const Checked checked = check_module("x = {}\n");

    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "need type annotation for \"x\"");
}

TEST(TypeChecker, ReportsABareEmptyContainerConstructorCall) {
    for (const std::string& ctor : {"list", "dict", "set", "frozenset", "tuple"}) {
        const Checked checked = check_module("x = " + ctor + "()\n");

        const diagnostics::Diagnostic error = only_error(checked);
        EXPECT_EQ(error.code, "TypeError") << "constructor: " << ctor;
        EXPECT_EQ(error.message, "need type annotation for \"x\"") << "constructor: " << ctor;
    }
}

// CONTAINER PARTIAL TYPES (Spec 5d, 2026-09-16). mypy does NOT take a
// variable's declared type from a first assignment of a bare empty
// container: it records a PARTIAL type and takes the type from whatever
// RESOLVES it. Every expected verdict below was produced by RUNNING mypy
// 1.18.1 and CPython 3.14.2, never by reasoning about the rule.
//
// All four accepting shapes are `mypy --strict` Success and CPython exit 0
// (measured: `[1]`, `{1: 2}`, `[1]`, `{1: 2}` respectively).
TEST(TypeChecker, AResolvedContainerPartialIsClean) {
    expect_clean("x = []\nx = [1]\nprint(x)\n");           // F1
    expect_clean("x = {}\nx = {1: 2}\nprint(x)\n");        // F2
    expect_clean("x = list()\nx = [1]\nprint(x)\n");       // F4
    expect_clean("x = dict()\nx = {1: 2}\nprint(x)\n");    // F5
}

// THE FOUR CALL SITES. The pre-pass has to run wherever a scope's body
// becomes current, and there are FOUR places that happens -- visit(Module),
// visit(FunctionDef)'s broken-method early-return branch, visit(FunctionDef)'s
// ordinary branch, and visit(ClassDef). A missing site shows up here and
// NOWHERE else: the module-scope test above passes with only the first wired.
// Measured, every shape below: mypy Success, CPython exit 0.
TEST(TypeChecker, AResolvedContainerPartialIsCleanAtFunctionScope) {
    expect_clean("def f() -> None:\n    x = []\n    x = [1]\n    print(x)\nf()\n");
    expect_clean("def f() -> None:\n    x = {}\n    x = {1: 2}\n    print(x)\nf()\n");
    expect_clean("def f() -> None:\n    x = list()\n    x = [1]\n    print(x)\nf()\n");
    expect_clean("def f() -> None:\n    x = dict()\n    x = {1: 2}\n    print(x)\nf()\n");
}

TEST(TypeChecker, AResolvedContainerPartialIsCleanAtNestedFunctionScope) {
    expect_clean("def o() -> None:\n    def i() -> None:\n        x = []\n"
                 "        x = [1]\n        print(x)\n    i()\no()\n");
    expect_clean("def o() -> None:\n    def i() -> None:\n        x = dict()\n"
                 "        x = {1: 2}\n        print(x)\n    i()\no()\n");
}

TEST(TypeChecker, AResolvedContainerPartialIsCleanInAMethodBody) {
    expect_clean("class C:\n    def m(self) -> None:\n        x = []\n"
                 "        x = [1]\n        print(x)\nC().m()\n");
}

TEST(TypeChecker, AResolvedContainerPartialIsCleanInAClassBody) {
    expect_clean("class C:\n    x = []\n    x = [1]\n");
    expect_clean("class C:\n    x = {}\n    x = {1: 2}\n");
}

// visit(FunctionDef)'s OTHER pre_bind_function_body call site: the
// report-and-return branch a method with no parameters at all takes. mypy
// reports ONLY `Method must have at least one argument` for this program
// (measured), so the partial must resolve there too rather than adding a
// second diagnostic of our own.
TEST(TypeChecker, AResolvedContainerPartialIsCleanInAZeroArgumentMethodBody) {
    const Checked checked = check_module(
        "class C:\n    def m() -> None:\n        x = []\n        x = [1]\n        print(x)\n");

    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "method must have at least one argument");
}

// A resolver at any block nesting depth still resolves. Measured, all three:
// mypy Success, CPython exit 0.
TEST(TypeChecker, AContainerPartialResolvedInsideABlockIsClean) {
    expect_clean("c: bool = True\nx = []\nif c:\n    x = [1]\nprint(x)\n");
    expect_clean("c: bool = False\nx = []\nwhile c:\n    x = [1]\nprint(x)\n");
    expect_clean("x = {}\nfor i in [1, 2]:\n    x = {i: i}\nprint(x)\n");
}

// DECISION 0 -- THE CONTROLS, which matter more than the fix. mypy genuinely
// reports `Need type annotation for "x"  [var-annotated]` for each of these
// (measured, every one), and this compiler is RIGHT today. A fix that closed
// the accepting shapes above by SUPPRESSING the diagnostic would silence a
// real error.
TEST(TypeChecker, AnUnresolvableContainerPartialStillReports) {
    // Each entry is a program mypy REJECTS with Need type annotation.
    const std::vector<std::pair<const char*, const char*>> shapes = {
        {"C1 no resolver anywhere", "x = []\nprint(x)\n"},
        {"C2 a read precedes the first resolver", "x = []\nprint(x)\nx = [1]\n"},
        {"C4 set() has no reachable resolver", "x = set()\nprint(x)\n"},
        {"C5 frozenset() has no reachable resolver", "x = frozenset()\nprint(x)\n"},
        // A tuple DISPLAY is a reachable non-empty value, so the kind
        // restriction -- not the read scan -- is the only thing keeping this
        // one reporting. Measured: mypy `Need type annotation for "x"`.
        {"tuple() is not a modelled partial", "x = tuple()\nx = (1,)\nprint(x)\n"},
        // A second bare empty container is a TOUCH that is not a resolver.
        {"a second bare display kills", "x = []\nx = []\nx = [1]\nprint(x)\n"},
        // A read through an assignment's right-hand side kills it too.
        {"y = x kills", "x = []\ny = x\nx = [1]\nprint(x)\nprint(y)\n"},
        // len(x) is a read. Measured: mypy `Need type annotation`.
        {"len(x) kills", "x = []\nprint(len(x))\nx = [1]\n"},
        // Iterating the partial is a read. Measured: mypy `Need type annotation`.
        {"for i in x kills", "x = []\nfor i in x:\n    print(i)\nx = [1]\n"},
        // A `for` TARGET rebinding the name is not a resolver. Measured: mypy
        // reports Need type annotation AND an incompatible assignment.
        {"a for target is not a resolver", "x = []\nfor x in [1, 2]:\n    pass\nprint(x)\n"},
        // A mismatched container kind is not a resolver. Measured: mypy
        // reports Need type annotation AND an incompatible assignment; this
        // compiler reports the first of the two.
        {"a dict display does not resolve a list partial", "x = []\nx = {1: 2}\nprint(x)\n"},
        // Nor does a value of a wholly unrelated type.
        {"None does not resolve a list partial", "x = []\nx = None\nprint(x)\n"},
    };
    for (const auto& [label, source] : shapes) {
        const Checked checked = check_module(source);

        const diagnostics::Diagnostic error = only_error(checked);
        EXPECT_EQ(error.code, "TypeError") << label;
        EXPECT_EQ(error.message, "need type annotation for \"x\"") << label;
        EXPECT_EQ(error.line, 1) << label;
    }
}

// The empty tuple DISPLAY is not a partial at all -- `tuple[()]` is a
// complete, non-generic type -- so this keeps its existing exact wording,
// which is mypy's own (measured verbatim).
TEST(TypeChecker, AnEmptyTupleDisplayFollowedByANonEmptyOneKeepsItsOwnWording) {
    const Checked checked = check_module("x = ()\nx = (1,)\nprint(x)\n");

    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message,
              "incompatible types in assignment (expression has type \"tuple[int]\", "
              "variable has type \"tuple[()]\")");
    EXPECT_EQ(error.line, 2);
}

// C2's sibling at a NESTED scope: mypy counts a read inside a nested `def`'s
// body as killing the partial even though that body does not run until later
// (measured -- `x = []` / `def f(): print(x)` / `x = [1]` / `f()` is
// `Need type annotation for "x"`, and CPython prints `[1]` at exit 0).
TEST(TypeChecker, AReadInsideANestedDefAboveTheResolverKillsThePartial) {
    const Checked checked =
        check_module("x = []\ndef f() -> None:\n    print(x)\nx = [1]\nf()\n");

    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "need type annotation for \"x\"");
    EXPECT_EQ(error.line, 1);
}

// The same read BELOW the resolver is harmless. Measured: mypy Success.
TEST(TypeChecker, AReadInsideANestedDefBelowTheResolverIsClean) {
    expect_clean("x = []\nx = [1]\ndef f() -> None:\n    print(x)\nf()\n");
}

// FIRST RESOLVER COMMITS: `x = [1]` makes `x` a `list[int]` for good, so the
// later `x = ["s"]` is checked against THAT and never draws a second
// `need type annotation`. The message is asserted verbatim because it turned
// out to match mypy's own text (measured -- mypy 1.18.1 says `List item 0 has
// incompatible type "str"; expected "int"  [list-item]` at line 3, and the
// only difference is this project's universal lower-case initial). The plan
// brief predicted a wording divergence here and there is none.
TEST(TypeChecker, TheFirstContainerResolverCommitsTheDeclaredType) {
    const Checked checked = check_module("x = []\nx = [1]\nx = [\"s\"]\nprint(x)\n");

    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "list item 0 has incompatible type \"str\"; expected \"int\"");
    EXPECT_EQ(error.line, 3);
}

// SPEC DECISION 4, and the test that catches a future round generalising
// `partial_none`'s union to containers. A CONTAINER partial resolves to the
// container type EXACTLY -- `list[int]`, never `list[int] | None`. Measured
// 2026-09-16: mypy reports ONLY `Incompatible types in assignment (expression
// has type "None", variable has type "list[int]")` here, with NO accompanying
// `Need type annotation`, because `x = [1]` already resolved the partial. So
// exactly ONE diagnostic is the sharp assertion, and the message is mypy's
// verbatim.
TEST(TypeChecker, AResolvedContainerPartialDoesNotAbsorbNone) {
    const Checked checked = check_module("x = []\nx = [1]\nx = None\nprint(x)\n");

    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message,
              "incompatible types in assignment (expression has type \"None\", "
              "variable has type \"list[int]\")");
    EXPECT_EQ(error.line, 3);
}

// SCOPE-LOCAL STATE MUST NOT LEAK. One function's cleared name must not clear
// a same-named partial in a sibling function. Measured: mypy reports exactly
// one error, at line 6 -- b's partial -- and CPython prints `[1]` then `[]`
// at exit 0.
TEST(TypeChecker, AClearedContainerPartialDoesNotLeakIntoASiblingScope) {
    const Checked checked =
        check_module("def a() -> None:\n    x = []\n    x = [1]\n    print(x)\n"
                     "def b() -> None:\n    x = []\n    print(x)\na()\nb()\n");

    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "need type annotation for \"x\"");
    EXPECT_EQ(error.line, 6);
}

// partial_none and partial_container are two DIFFERENT rules with two
// different resolved types, and they can never both be live on one binding:
// a `None` value and a bare-empty-container value are mutually exclusive at
// the one creation site. `x = None` / `x = []` is an existing MISSED error
// (mypy: `Need type annotation for "x"` at line 2; this compiler: silent),
// deliberately out of scope per the spec -- pinned so a future round finds it
// recorded rather than surprising.
TEST(TypeChecker, AnEmptyContainerResolvingANonePartialIsAKnownMissedError) {
    expect_clean("x = None\nx = []\nprint(x)\n");
}

// THE SECOND RESOLVER FORM (Spec 5d §4 item 2): a DICT SUBSCRIPT STORE,
// `x[k] = v`, resolves a live dict partial to `dict[K, V]` taken from the key
// and value expression types. Measured 2026-09-16: mypy 1.18.1 Success and
// CPython 3.14.2 exit 0 printing `{'a': 1}`, `{'a': 1, 'b': 2}` and
// `{'z': 9}` respectively, where this compiler reported a false
// `TypeError: need type annotation for "x"` at 1:1 for all three.
TEST(TypeChecker, ADictSubscriptStoreResolvesAContainerPartial) {
    expect_clean("x = {}\nx[\"a\"] = 1\nprint(x)\n");                      // F3
    // The FIRST store commits; a second, compatible store is checked
    // against it and never draws a second diagnostic.
    expect_clean("x = {}\nx[\"a\"] = 1\nx[\"b\"] = 2\nprint(x)\n");
    // The STORE committed the type, so the later whole-dict assignment is
    // checked against `dict[str, int]` and fits.
    expect_clean("x = {}\nx[\"a\"] = 1\nx = {\"z\": 9}\nprint(x)\n");
}

// THE FOUR CALL SITES AGAIN, for the new resolver form specifically. A
// resolver form wired only at module scope would be a half-landing of exactly
// the kind the plan names, and the module-scope test above cannot see it.
// Measured, every shape: mypy Success, CPython exit 0 printing `{'a': 1}`.
TEST(TypeChecker, ADictSubscriptStoreResolvesAtEveryScope) {
    expect_clean("def f() -> None:\n    x = {}\n    x[\"a\"] = 1\n    print(x)\nf()\n");
    expect_clean("def o() -> None:\n    def i() -> None:\n        x = {}\n"
                 "        x[\"a\"] = 1\n        print(x)\n    i()\no()\n");
    expect_clean("class C:\n    def m(self) -> None:\n        x = {}\n"
                 "        x[\"a\"] = 1\n        print(x)\nC().m()\n");
    expect_clean("class C:\n    x = {}\n    x[\"a\"] = 1\nprint(C.x)\n");
    // A resolver at block nesting depth still resolves, exactly as the
    // display form does.
    expect_clean("c: bool = True\nx = {}\nif c:\n    x[\"a\"] = 1\nprint(x)\n");
}

// CONTROL C3, THE ASYMMETRY, and the single most important control of this
// task: a LIST subscript store is NOT a resolver, where a dict one is.
// Measured both ways against mypy 1.18.1 -- `x = []` / `x[0] = 1` is
// `Need type annotation for "x" (hint: "x: list[<type>] = ...")` while
// `x = {}` / `x["a"] = 1` is Success. This is mypy's rule, not a modelling
// convenience, and this test FAILS the moment a list store is made to
// resolve.
TEST(TypeChecker, AListSubscriptStoreIsNotAResolver) {
    for (const char* source : {"x = []\nx[0] = 1\n", "x = []\nx[0] = 1\nprint(x)\n",
                               "def f() -> None:\n    x = []\n    x[0] = 1\nf()\n"}) {
        const Checked checked = check_module(source);

        const diagnostics::Diagnostic error = only_error(checked);
        EXPECT_EQ(error.code, "TypeError") << source;
        EXPECT_EQ(error.message, "need type annotation for \"x\"") << source;
    }
    // THE ASYMMETRY IN ONE PAIR: the same store, the same index, the same
    // stored value, differing ONLY in which container the partial is.
    // Measured: `x = {}` / `x[0] = 1` is mypy Success and CPython prints
    // `{0: 1}`, while the `x = []` spelling on the first line of the loop
    // above is `Need type annotation`.
    expect_clean("x = {}\nx[0] = 1\nprint(x)\n");
}

// FIRST-STORE-COMMITS, the conflicting halves. Both programs are rejected by
// mypy and run under CPython, so a VERDICT is what matters -- but the
// messages are asserted verbatim anyway, because both turned out to match
// mypy 1.18.1's own text modulo this project's lower-case initial (measured:
// `Incompatible types in assignment (expression has type "str", target has
// type "int")` and `Invalid index type "str" for "dict[int, int]"; expected
// type "int"`, both at line 3 -- the second differs only in dropping mypy's
// `; expected type "int"` suffix). Spec §7 predicted a wording divergence
// here and, as with the display resolver, there is almost none.
TEST(TypeChecker, AConflictingLaterDictStoreReportsAgainstTheCommittedType) {
    const Checked value_conflict =
        check_module("x = {}\nx[\"a\"] = 1\nx[\"b\"] = \"s\"\nprint(x)\n");
    const diagnostics::Diagnostic value_error = only_error(value_conflict);
    EXPECT_EQ(value_error.code, "TypeError");
    EXPECT_EQ(value_error.message,
              "incompatible types in assignment (expression has type \"str\", "
              "target has type \"int\")");
    // Never a second `need type annotation` at line 1: the store committed
    // the type, so the conflict is reported against THAT.
    EXPECT_EQ(value_error.line, 3);

    const Checked key_conflict = check_module("x = {}\nx[1] = 1\nx[\"a\"] = 2\nprint(x)\n");
    const diagnostics::Diagnostic key_error = only_error(key_conflict);
    EXPECT_EQ(key_error.code, "TypeError");
    EXPECT_EQ(key_error.message, "invalid index type \"str\" for \"dict[int, int]\"");
    EXPECT_EQ(key_error.line, 3);
}

// A later WHOLE-container assignment is checked against the type the STORE
// committed, in both the mismatched-kind and the None direction -- the
// subscript-store sibling of AResolvedContainerPartialDoesNotAbsorbNone, and
// the test that would catch a store resolving to `dict[str, int] | None`.
// Measured, both messages verbatim from mypy 1.18.1 at line 3, both CPython
// exit 0.
TEST(TypeChecker, AStoreResolvedPartialIsACeilingForLaterAssignments) {
    const Checked to_list = check_module("x = {}\nx[\"a\"] = 1\nx = [1]\nprint(x)\n");
    const diagnostics::Diagnostic list_error = only_error(to_list);
    EXPECT_EQ(list_error.code, "TypeError");
    EXPECT_EQ(list_error.message,
              "incompatible types in assignment (expression has type \"list[int]\", "
              "variable has type \"dict[str, int]\")");
    EXPECT_EQ(list_error.line, 3);

    const Checked to_none = check_module("x = {}\nx[\"a\"] = 1\nx = None\nprint(x)\n");
    const diagnostics::Diagnostic none_error = only_error(to_none);
    EXPECT_EQ(none_error.code, "TypeError");
    EXPECT_EQ(none_error.message,
              "incompatible types in assignment (expression has type \"None\", "
              "variable has type \"dict[str, int]\")");
    EXPECT_EQ(none_error.line, 3);
}

// KNOWN MISSED ERRORS the subscript-store resolver adds, pinned so a future
// round finds them recorded rather than surprising. In each, mypy reports
// `Need type annotation for "x"` (or an incompatible assignment) and this
// compiler is now SILENT; CPython runs all four at exit 0, so these are the
// mypy-only half of the union rule -- the sanctioned direction, but real.
//
// EVERY ONE IS A NEW INSTANCE OF AN EXISTING CLASS, not a new class: the
// display resolver landed in f2e62cf has the identical silence for each,
// verified by measuring its own spelling of the same shape (`x = []` /
// `if False: x = [1]`, `x = []` / `return` / `x = [1]`, and `x = []` /
// `x = [len]` / `x = [1]`, all mypy-reject and all already silent before this
// change). The cause is the pre-pass mechanism itself: the scan is purely
// SYNTACTIC, so it models neither mypy's `{False, 0, None}` constant folding
// nor reachability, and it has no TYPES, so it cannot see that a resolver
// leaves an Unknown element behind. Closing them needs a
// reachability/constant-folding model this codebase does not have -- and
// CLAUDE.md's own `{False, 0, None}` entry records why a half-measured one is
// worse than none (`if "":` must NOT be pruned).
TEST(TypeChecker, ADictStoreInAnUnreachableOrUntypedPositionIsAKnownMissedError) {
    // mypy: `Need type annotation for "x"` at line 1 -- the `if False:` body
    // is pruned, so mypy never sees the resolver. CPython prints `{}`.
    expect_clean("x = {}\nif False:\n    x[\"a\"] = 1\nprint(x)\n");
    // mypy: same, at line 2 -- the store is unreachable after the `return`.
    expect_clean("def f() -> None:\n    x = {}\n    return\n    x[\"a\"] = 1\nf()\n");
    // An Unknown KEY or VALUE (a bare builtin function resolves to Unknown,
    // which is absorbing) freezes `dict[Unknown, int]` / `dict[str, Unknown]`
    // in place, so the later whole-dict assignment is unchecked. mypy reports
    // a `[dict-item]` error at line 3 for both.
    expect_clean("x = {}\nx[len] = 1\nx = {\"z\": 9}\nprint(x)\n");
    expect_clean("x = {}\nx[\"a\"] = len\nx = {\"z\": 9}\nprint(x)\n");
}

// A store into a subscript of a subscript is not a resolver either -- the
// receiver is not a plain Name, so the scan reads it as an ordinary touch.
// Measured: mypy reports `Need type annotation for "x"` at line 1 and CPython
// raises `KeyError: 'a'` at exit 1, so BOTH oracles reject this program.
TEST(TypeChecker, AStoreIntoASubscriptOfASubscriptIsNotAResolver) {
    const Checked checked = check_module("x = {}\nx[\"a\"][\"b\"] = 1\nprint(x)\n");

    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "need type annotation for \"x\"");
    EXPECT_EQ(error.line, 1);
}

// CONTROL: the ANNOTATED path is untouched. `x: dict[str, int] = {}` is not a
// partial at all (the annotation supplies the declared type), so the store
// goes through the ordinary subscript-store check. Measured: mypy Success,
// CPython prints `{'a': 1}`; and the incompatible sibling still reports.
TEST(TypeChecker, AStoreIntoAnAnnotatedDictIsUnaffected) {
    expect_clean("x: dict[str, int] = {}\nx[\"a\"] = 1\nprint(x)\n");

    const Checked checked =
        check_module("x: dict[str, int] = {}\nx[\"a\"] = \"s\"\nprint(x)\n");
    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.line, 2);
}

// THE ORDERING RULE, module scope. Verified: used-before-def.
TEST(TypeChecker, ReportsAModuleLevelUseBeforeDefinition) {
    const Checked checked = check_module("y = x\nx = 5\n");

    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "NameError");
    EXPECT_EQ(error.message, "name 'x' is used before definition");
    EXPECT_EQ(error.line, 1);
}

// >= not >: the right-hand side is evaluated before the target is bound.
TEST(TypeChecker, AReadOnItsOwnBindingLineIsAViolation) {
    const Checked checked = check_module("x = x + 1\n");

    EXPECT_EQ(only_error(checked).code, "NameError");
}

TEST(TypeChecker, ReportsAnUndefinedName) {
    const Checked checked = check_module("y = nope\n");

    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "NameError");
    EXPECT_EQ(error.message, "name 'nope' is not defined");
}

// Verified: `class C(Generic[T])` parses, and Generic cannot be imported in
// this subset, so NameError is the CORRECT outcome for a program nobody can
// legally write. Spec 5a predicted this test.
TEST(TypeChecker, ReportsGenericAsUndefinedBecauseItCannotBeImported) {
    const Checked checked = check_module("class C(Generic):\n    pass\n");

    EXPECT_EQ(only_error(checked).code, "NameError");
}

// Two-pass, phase 2 depending on phase 1. Verified mypy-clean, unquoted,
// under PEP 649. A SINGLE-phase collect fails this.
TEST(TypeChecker, AnAnnotationMayNameAClassDeclaredLater) {
    expect_clean("def f(a: B) -> None:\n    pass\nclass B:\n    pass\n");
    expect_clean("class A:\n    x: B\nclass B:\n    pass\n");
}

// This test was VACUOUS at one point -- back then `return g` walked to
// RecursiveVisitor::visit(Return), which visits the Name `g` via accept(),
// and TypeChecker's Name arm was RecursiveVisitor's own no-op default (Name
// carries nothing to check), so `g` was never typed at all and the test
// could not have failed regardless of what the checker did. `print(g)` was
// added alongside it then, since ExprStmt IS overridden and actually reaches
// ExpressionTyper::type_of_name. This also needed TypeChecker::visit(FunctionDef)
// to push a Function scope (see type_checker.h/.cpp) -- without it, the body
// was checked in the still-current Module scope, so this read resolved
// in_own_scope == true and falsely reported "used before definition" against
// `g`'s later module-level binding.
//
// `return g` is no longer the vacuous half this
// comment used to describe -- Return is now a real, overridden arm, so `g`
// in `return g` is typed through ExpressionTyper exactly like `print(g)`
// already was. Both forms verified mypy-clean.
TEST(TypeChecker, AFunctionBodySeesGlobalsDefinedBelowIt) {
    expect_clean("def f() -> int:\n    print(g)\n    return g\ng: int = 5\n");
    expect_clean("def f() -> None:\n    print(x)\nx = 5\n");
}

// The in_own_scope gate itself (ExpressionTyper's
// Name arm) and statement_line_'s INT_MAX default had zero DIRECT coverage
// until this test added a Function scope to check against -- every existing
// ordering test ran at module scope, where the reader's own scope IS the
// binding's scope, so in_own_scope was always true and the exemption branch
// never ran. This test puts the exact same construct in both scope shapes:
// an outward read (function reading a module global bound later) is exempt,
// but the identical own-scope shape (module scope reading its own name bound
// later) is not.
TEST(TypeChecker, AnOutwardReadIsExemptFromTheOrderingCheckButAnOwnScopeReadIsNot) {
    expect_clean("def f() -> None:\n    print(x)\nx: int = 5\n");

    const Checked checked = check_module("print(y)\ny: int = 5\n");
    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "NameError");
    EXPECT_EQ(error.message, "name 'y' is used before definition");
}

// mypy does NO definite-assignment analysis for variables by default:
// possibly-undefined is off even under --strict. Building this check would be
// a false-positive generator.
TEST(TypeChecker, DoesNotReportAPossiblyUndefinedVariable) {
    expect_clean("c: bool = True\nif c:\n    v = 1\nprint(v)\n");
}

TEST(TypeChecker, ChecksSubscriptAndAttributeStores) {
    const Checked bad_item = check_module("xs: list[int] = [1]\nxs[0] = \"s\"\n");
    EXPECT_EQ(only_error(bad_item).code, "TypeError");

    expect_clean("xs: list[int] = [1]\nxs[0] = 2\n");
}

TEST(TypeChecker, BindsTupleUnpackingElementWise) {
    expect_clean("a, b = 1, \"s\"\nc: int = a\nd: str = b\n");
}

TEST(TypeChecker, PopulatesTheTypeMapForExpressionsOnly) {
    const Checked checked = check_module("x: int = 5\n");

    // The Constant 5 only. The AnnAssign is a statement, and both the target
    // Name and the annotation Name are annotation-or-target positions with no
    // runtime value type.
    EXPECT_EQ(checked.typed_expressions, 1u);
}

// ---------------------------------------------------------------------------
// Task 18: FunctionDef.
// ---------------------------------------------------------------------------

// Verified: --strict's disallow-untyped-defs/disallow-incomplete-defs. A
// top-level function is never a method, so no self exemption applies.
TEST(TypeChecker, AMissingParameterAnnotationIsAnError) {
    const Checked checked = check_module("def f(x) -> None:\n    pass\n");

    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "function is missing a type annotation");
}

TEST(TypeChecker, AMissingReturnAnnotationIsAnError) {
    const Checked checked = check_module("def f(x: int):\n    pass\n");

    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "function is missing a type annotation");
}

TEST(TypeChecker, AFullyAnnotatedFunctionIsClean) {
    expect_clean("def f(x: int) -> int:\n    return x\n");
}

// The correction to the brief. If this test fails, the implementation is
// requiring -> None; that is a FALSE error on a mypy-clean program.
TEST(TypeChecker, InitNeedsNoReturnAnnotationWhenAParameterIsAnnotated) {
    expect_clean("class C:\n    def __init__(self, a: int):\n        self.a = a\n");
}

TEST(TypeChecker, AFullyUnannotatedInitIsStillAnError) {
    const Checked checked = check_module("class C:\n    def __init__(self):\n        pass\n");
    EXPECT_EQ(only_error(checked).code, "TypeError");
}

TEST(TypeChecker, AnOrdinaryMethodStillNeedsAReturnAnnotation) {
    const Checked checked = check_module("class C:\n    def m(self, a: int):\n        pass\n");
    EXPECT_EQ(only_error(checked).code, "TypeError");
}

// Verified: mypy reports "Method must have at least one argument. Did you
// forget the "self" argument?" at the definition (and again at each call
// site, which we do not repeat). Reported ONCE, here, at the def line.
TEST(TypeChecker, AMethodWithNoParametersIsReportedOnceAtTheDefinition) {
    const Checked checked = check_module("class C:\n    def m():\n        pass\n");

    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "method must have at least one argument");
    EXPECT_EQ(error.line, 2);
}

// Verified: mypy's code is `assignment`, not `arg-type`, for a wrong-typed
// default; reported at the `def` line.
TEST(TypeChecker, AWrongTypedDefaultIsReportedAtTheDefLine) {
    const Checked checked = check_module("def f(x: int = \"s\") -> None:\n    pass\n");

    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message,
              "incompatible default for argument \"x\" (default has type \"str\", "
              "argument has type \"int\")");
    EXPECT_EQ(error.line, 1);
}

// EXTENDED. This test used to only DEFINE `def f(x: int = 5)` and never call
// it, which is why the arity check ignoring defaults entirely had zero
// coverage: `Type::callable` carried no notion of an optional parameter, so
// every call omitting one was "too few arguments for ..." -- a false
// TypeError on about as ordinary a Python program as exists. The CALL is the
// whole point of the test now.
TEST(TypeChecker, ACompatibleDefaultIsCleanAndMayBeOmittedAtTheCall) {
    expect_clean("def f(x: int = 5) -> None:\n    pass\n");
    expect_clean("def log(msg: str, level: int = 1) -> None:\n"
                 "    print(msg)\n"
                 "    print(level)\n"
                 "\n"
                 "\n"
                 "log(\"start\")\n"
                 "log(\"start\", 2)\n");
}

// A method's and a constructor's defaults, which travel by two different
// routes -- the method signature is stored in ClassTable and has self erased
// at the attribute access, the constructor is REBUILT by
// ClassTable::constructor_type from __init__ minus self -- so a
// defaulted-parameter count that survived one could still be dropped by the
// other.
TEST(TypeChecker, AMethodAndAConstructorDefaultMayBeOmittedAtTheCall) {
    expect_clean("class G:\n"
                 "    def __init__(self, name: str = \"world\") -> None:\n"
                 "        self.name = name\n"
                 "\n"
                 "    def greet(self, punct: str = \"!\") -> str:\n"
                 "        return self.name + punct\n"
                 "\n"
                 "\n"
                 "g = G()\n"
                 "print(g.greet())\n"
                 "print(G(\"ann\").greet(\"?\"))\n");
}

// Verified against mypy 1.18.1: all four of these are `Success`.
TEST(TypeChecker, AnExceptionSubclassAcceptsAnyConstructorArity) {
    expect_clean("class MyError(Exception):\n"
                 "    pass\n"
                 "def f() -> None:\n"
                 "    print(MyError())\n"
                 "    print(MyError(\"boom\"))\n"
                 "    print(MyError(\"boom\", 42))\n"
                 "    print(ValueError(\"bad\", 1, 2))\n");
}

// Unchanged: a plain class still checks its arity. Verified against mypy
// 1.18.1: `Too many arguments for "Plain" [call-arg]`.
TEST(TypeChecker, APlainClassStillChecksConstructorArity) {
    const Checked checked = check_module("class Plain:\n"
                                         "    pass\n"
                                         "p = Plain(\"x\")\n");
    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "too many arguments for \"Plain\"");
}

// An inherited __init__ is the subclass's constructor, with the parameter and
// the return both rebound to the subclass. Verified against mypy 1.18.1:
// reveal_type(Child) is `def (n: builtins.int) -> Child`,
// `Child()` is `Missing positional argument "n" in call to "Child"` and
// `Child("s")` is `Argument 1 to "Child" has incompatible type "str";
// expected "int"`. This ALREADY works -- the guard is here so a later change
// to the chain walk cannot silently take it away.
TEST(TypeChecker, AnInheritedConstructorIsCheckedAgainstItsParameters) {
    const Checked checked = check_module("class Parent:\n"
                                         "    def __init__(self, n: int) -> None:\n"
                                         "        self.n = n\n"
                                         "class Child(Parent):\n"
                                         "    pass\n"
                                         "a: Child = Child(1)\n"
                                         "b: Child = Child(\"s\")\n"
                                         "print(a, b)\n");
    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message,
              "argument 1 to \"Child\" has incompatible type \"str\"; expected \"int\"");
}

// A class based on a builtin KIND with no __init__ of its own inherits a
// BOUNDED overload set (see ClassTable::ConstructorCheck), so a call with
// arguments is deferred rather than either checked or silently accepted.
// Measured against mypy 1.18.1 and CPython: `class MyInt(int): pass` then
// MyInt(3) is `Success` and prints 3, and MyInt("ff", 16) is `Success` and
// prints 255 -- both of which an arity check on the modelled nullary
// constructor called a false "too many arguments" -- while MyInt(1, 2, 3) is
// `No overload variant of "MyInt" matches argument types "int", "int", "int"
// [call-overload]` and dies with `TypeError: int() takes at most 2 arguments
// (3 given)`. One diagnostic covers all three honestly; silence would accept
// the third, which both oracles reject.
TEST(TypeChecker, ABuiltinBasedSubclassDefersItsConstructorArguments) {
    const Checked checked = check_module("class MyInt(int):\n"
                                         "    pass\n"
                                         "print(MyInt(3))\n");
    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "NotImplementedError");
    EXPECT_EQ(error.message,
              "calls to 'MyInt', which inherits an overloaded builtin constructor, are not "
              "supported");

    const Checked too_many = check_module("class MyInt(int):\n"
                                          "    pass\n"
                                          "print(MyInt(1, 2, 3))\n");
    EXPECT_EQ(only_error(too_many).code, "NotImplementedError");

    const Checked str_based = check_module("class C(str):\n"
                                           "    pass\n"
                                           "print(C(\"abc\"))\n");
    EXPECT_EQ(only_error(str_based).code, "NotImplementedError");
}

// With NO arguments there is no overload set to be unable to spell, so the
// modelled constructor is checked as usual and stays silent. Measured:
// `class MyInt(int): pass` then MyInt() is mypy `Success` and prints 0.
TEST(TypeChecker, ABuiltinBasedSubclassConstructedWithNoArgumentsIsClean) {
    expect_clean("class MyInt(int):\n"
                 "    pass\n"
                 "print(MyInt())\n");
}

// A parametric builtin base reaches the same rule: the base is recorded as
// `list[int]`, but ClassTable::base_key still keys it under the bare "list"
// spelling for the chain walk, so the constructor question sees straight
// through the type argument. Measured against mypy 1.18.1 and CPython:
// `class IntList(list[int]): pass` then IntList([1, 2]) is `Success` and
// prints [1, 2], while IntList(1, 2, 3) is `No overload variant of "IntList"
// matches argument types "int", "int", "int"  [call-overload]` and dies with
// `TypeError: list expected at most 1 argument, got 3`.
TEST(TypeChecker, AParametricBuiltinBasedSubclassDefersItsConstructorArguments) {
    const Checked checked = check_module("class IntList(list[int]):\n"
                                         "    pass\n"
                                         "print(IntList([1, 2]))\n");
    EXPECT_EQ(only_error(checked).code, "NotImplementedError");

    const Checked too_many = check_module("class IntList(list[int]):\n"
                                          "    pass\n"
                                          "print(IntList(1, 2, 3))\n");
    EXPECT_EQ(only_error(too_many).code, "NotImplementedError");
}

// The constructor search follows base ORDER, as Python's MRO does. With a
// builtin base to the LEFT of a plain base that declares __init__, the
// builtin wins and the plain base's parameter types are never applied.
// Measured against mypy 1.18.1 and CPython: this exact program is `Success`
// and prints 3, where the whole-chain search reported a false
// `argument 1 to "MyInt" has incompatible type "int"; expected "str"`.
TEST(TypeChecker, ABuiltinBaseLeftOfADeclaredInitDefersTheConstructor) {
    const Checked checked = check_module("class Mixin:\n"
                                         "    def __init__(self, a: str) -> None:\n"
                                         "        print(a)\n"
                                         "class MyInt(int, Mixin):\n"
                                         "    pass\n"
                                         "print(MyInt(3))\n");
    EXPECT_EQ(only_error(checked).code, "NotImplementedError");
}

// The complementary order still uses the plain base's __init__. Measured
// against mypy 1.18.1: with the bases swapped, MyInt("s") is `Success` and
// MyInt(3) is `Argument 1 to "MyInt" has incompatible type "int"; expected
// "str"  [arg-type]`.
TEST(TypeChecker, ADeclaredInitLeftOfABuiltinBaseStillChecksItsParameters) {
    expect_clean("class Mixin:\n"
                 "    def __init__(self, a: str) -> None:\n"
                 "        print(a)\n"
                 "class MyInt(Mixin, int):\n"
                 "    pass\n"
                 "print(MyInt(\"s\"))\n");

    const Checked checked = check_module("class Mixin:\n"
                                         "    def __init__(self, a: str) -> None:\n"
                                         "        print(a)\n"
                                         "class MyInt(Mixin, int):\n"
                                         "    pass\n"
                                         "print(MyInt(3))\n");
    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message,
              "argument 1 to \"MyInt\" has incompatible type \"int\"; expected \"str\"");
}

// The same order rule with an exception base, where the answer is silence
// rather than a deferral. Measured against mypy 1.18.1 and CPython: this
// program is `Success` and prints ('boom', 42), where the whole-chain search
// reported a false `too many arguments for "MyErr"`.
TEST(TypeChecker, AnExceptionBaseLeftOfADeclaredInitAcceptsAnyArity) {
    expect_clean("class Mixin:\n"
                 "    def __init__(self, a: str) -> None:\n"
                 "        print(a)\n"
                 "class MyErr(Exception, Mixin):\n"
                 "    pass\n"
                 "print(MyErr(\"boom\", 42))\n");
}

// And the deeper case still CHECKS: a declared __init__ two levels up the
// leftmost base is reached before anything reaches BaseException. Verified
// against mypy 1.18.1: `Leaf("s")` is `Argument 1 to "Leaf" has incompatible
// type "str"; expected "int"  [arg-type]`, and `Leaf(1)` is `Success`.
TEST(TypeChecker, ADeclaredInitTwoLevelsUpBeatsAnExceptionBase) {
    const Checked checked = check_module("class Grand:\n"
                                         "    def __init__(self, a: int) -> None:\n"
                                         "        print(a)\n"
                                         "class Mid(Grand):\n"
                                         "    pass\n"
                                         "class Leaf(Mid, Exception):\n"
                                         "    pass\n"
                                         "print(Leaf(\"s\"))\n");
    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message,
              "argument 1 to \"Leaf\" has incompatible type \"str\"; expected \"int\"");

    expect_clean("class Grand:\n"
                 "    def __init__(self, a: int) -> None:\n"
                 "        print(a)\n"
                 "class Mid(Grand):\n"
                 "    pass\n"
                 "class Leaf(Mid, Exception):\n"
                 "    pass\n"
                 "print(Leaf(1))\n");
}

// The other direction must NOT be lost: too MANY arguments is still an error,
// and so is omitting a parameter that has no default. Verified against mypy
// 1.18.1, which reports both.
TEST(TypeChecker, DefaultsDoNotSilenceARealArityError) {
    const Checked too_many =
        check_module("def log(msg: str, level: int = 1) -> None:\n"
                     "    print(msg)\n"
                     "    print(level)\n"
                     "\n"
                     "\n"
                     "log(\"a\", 2, 3)\n");
    const diagnostics::Diagnostic too_many_error = only_error(too_many);
    EXPECT_EQ(too_many_error.code, "TypeError");
    EXPECT_EQ(too_many_error.message, "too many arguments for \"log\"");

    const Checked too_few =
        check_module("def log(msg: str, level: int = 1) -> None:\n"
                     "    print(msg)\n"
                     "    print(level)\n"
                     "\n"
                     "\n"
                     "log()\n");
    const diagnostics::Diagnostic too_few_error = only_error(too_few);
    EXPECT_EQ(too_few_error.code, "TypeError");
    EXPECT_EQ(too_few_error.message, "too few arguments for \"log\"");
}

// A SUPPLIED argument is still checked against its parameter even when that
// parameter has a default -- the per-argument loop's bound changed with this
// fix, so this is the case that would silently stop being checked.
TEST(TypeChecker, ASuppliedArgumentForADefaultedParameterIsStillTypeChecked) {
    const Checked checked =
        check_module("def log(msg: str, level: int = 1) -> None:\n"
                     "    print(msg)\n"
                     "    print(level)\n"
                     "\n"
                     "\n"
                     "log(\"a\", \"b\")\n");
    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message,
              "argument 2 to \"log\" has incompatible type \"str\"; expected \"int\"");
}

// Verified clean: module-level names are visible ahead of their definition
// inside function bodies, and the function's own name is one of them.
//
// PLAN DEFECT (reported, not silently patched): the brief's own version of
// this test used `return f(n)`, which was VACUOUS AT THE TIME -- Return was
// not yet overridden (that landed in Task 20) and RecursiveVisitor's default
// just walked to the Name "f", whose own visit() is RecursiveVisitor's no-op
// default, so nothing was ever typed through ExpressionTyper and the test
// would have passed regardless of whether recursion resolution worked at
// all. This is the exact same class of defect already found
// and fixed for `return g` (rewritten to `print(g)`). Rewritten the same way
// here, inside the `if`, so the call genuinely reaches ExpressionTyper via
// the TypeChecker-overridden ExprStmt arm -- left as-is now that Return IS
// real, since `return 0` right below already exercises a genuine typed
// Return, and this test's own point is recursion, not Return.
TEST(TypeChecker, DirectRecursionIsClean) {
    expect_clean("def f(n: int) -> int:\n    if n:\n        print(f(n))\n    return 0\n");
}

// Verified: calling a nested function defined LATER in the same body is
// used-before-def -- a nested def is bound at its lexical position, so a
// Function scope gets NO collect pass.
TEST(TypeChecker, ANestedDefIsNotHoisted) {
    const Checked checked = check_module(
        "def outer() -> int:\n"
        "    r: int = inner()\n"
        "    def inner() -> int:\n"
        "        return 1\n"
        "    return r\n");

    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "NameError");
    EXPECT_EQ(error.message, "name 'inner' is used before definition");
}

// Verified CLEAN, and it looks contradictory next to the test above: a CALL
// happens now, a closure READ happens at call time. The unifying rule is
// that only same-scope reads are order-checked.
//
// Task 18 rewrote the brief's own `return v` to `print(v)` here, since
// Return was not yet overridden then and `return v` was the SAME vacuous
// shape as DirectRecursionIsClean above -- `v` was never typed through
// ExpressionTyper either way. Task 20 makes Return real, which flips this
// fixture over to a NEW failure: `i` declares `-> int` but (with `print(v)`
// as its only statement) never returns at all, which is a genuine missing
// return statement -- verified against mypy 1.18.1, which reports exactly
// that (`error: Missing return statement  [return]`) on this fixture
// unmodified. Restored to the brief's original `return v`, which both
// supplies the now-required return AND exercises the exact same closure
// read this test is named for (typed via ExpressionTyper same as `print(v)`
// would have been, now that Return is no longer inert).
TEST(TypeChecker, AClosureMayReadALocalAssignedAfterItsOwnDef) {
    expect_clean(
        "def o() -> int:\n"
        "    def i() -> int:\n"
        "        return v\n"
        "    v: int = 1\n"
        "    return i()\n");
}

TEST(TypeChecker, AParameterAnnotationDeclaresTheNameForTheWholeBody) {
    const Checked checked =
        check_module("def f(x: int) -> None:\n    x = \"s\"\n");
    EXPECT_EQ(only_error(checked).code, "TypeError");
}

// A local variable assigned later in the SAME function body is the function-
// scope analogue of ReportsAModuleLevelUseBeforeDefinition -- exercises
// pre_bind_function_body's Assign-target placeholder.
TEST(TypeChecker, AFunctionLocalUseBeforeDefinitionIsAViolation) {
    const Checked checked = check_module("def f() -> None:\n    print(x)\n    x = 5\n");

    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "NameError");
    EXPECT_EQ(error.message, "name 'x' is used before definition");
}

// pre_bind_function_body did not recurse into If/While/For bodies, so a name
// whose only forward binding in a function's own scope sat inside one of
// those was silently treated as never-local at all -- an ENCLOSING binding of
// the same name is essential to this reproduction (see the comment on
// for_each_own_scope_binding), since without one the read would fall through
// to the (already-correct) "not defined" case and mask the defect entirely.
// Measured against mypy 1.18.1 and CPython 3.14 (driven with `outer()`
// called at module level, `outer` binding `x` at module scope so the read
// resolves to something rather than nothing): mypy `Name "x" is used before
// definition  [used-before-def]`, CPython `UnboundLocalError: cannot access
// local variable 'x' where it is not associated with a value` -- both
// oracles reject a program this compiler used to accept silently.
TEST(TypeChecker, AReadBeforeAConditionallyAssignedFunctionLocalIsAViolation) {
    const Checked checked = check_module("x: int = 1\n"
                                         "def f(c: bool) -> None:\n"
                                         "    print(x)\n"
                                         "    if c:\n"
                                         "        x = 5\n"
                                         "f(True)\n");

    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "NameError");
    EXPECT_EQ(error.message, "name 'x' is used before definition");
    EXPECT_EQ(error.line, 3);
}

// The exact shape CLAUDE.md recorded as open: a `for` target is a binding
// form pre_bind_function_body did not recognise at all (recursion aside), so
// a read of a name whose only forward binding is a LATER `for`-loop rebind
// was invisible even with no control-flow nesting in play. Measured against
// mypy 1.18.1 and CPython 3.14 (driven with `b = Bag(); b.m()`): mypy `Name
// "self" is used before definition  [used-before-def]` at the store inside
// `inner`, CPython `UnboundLocalError: cannot access local variable 'self'
// where it is not associated with a value` at the same line.
TEST(TypeChecker, AForTargetRebindMakesAnEarlierReadInTheSameScopeAViolation) {
    const Checked checked = check_module("class Bag:\n"
                                         "    def m(self) -> None:\n"
                                         "        def inner() -> None:\n"
                                         "            self.q = 1\n"
                                         "            for self in [1, 2]:\n"
                                         "                pass\n"
                                         "        inner()\n");

    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "NameError");
    EXPECT_EQ(error.message, "name 'self' is used before definition");
    EXPECT_EQ(error.line, 4);
}

// A nested `def`'s own name is likewise pre_bind_function_body's business,
// and recursion makes it reachable from inside an `if` too -- both the
// binding form and the control-flow recursion are exercised by one fixture.
// An ENCLOSING `g` (module-level) is required for the same reason as the
// Assign case above. Measured against mypy 1.18.1 and CPython 3.14 (driven
// with `outer(True)`): mypy `Name "g" is used before definition
// [used-before-def]`, CPython `UnboundLocalError`.
TEST(TypeChecker, AConditionalNestedDefMakesAnEarlierReadAViolation) {
    const Checked checked = check_module("def g() -> int:\n"
                                         "    return 0\n"
                                         "def outer(c: bool) -> None:\n"
                                         "    print(g)\n"
                                         "    if c:\n"
                                         "        def g() -> int:\n"
                                         "            return 1\n"
                                         "    print(g)\n"
                                         "outer(True)\n");

    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "NameError");
    EXPECT_EQ(error.message, "name 'g' is used before definition");
    EXPECT_EQ(error.line, 4);
}

// CONTROL, and the one this fix is most likely to break if it goes too far:
// a read that resolves OUTWARD to an ENCLOSING scope, with no local rebind
// anywhere in the reader's own scope, must stay clean -- mypy's rule is that
// an outward-resolving read is never order-checked, and pre_bind_function_body
// must not manufacture a placeholder for a name this scope never itself
// binds. This is the closure fixture AClosureMayReadALocalAssignedAfterItsOwnDef
// already pins elsewhere in this file; repeated here as the direct control
// for THIS fix, differing from the violation tests above in exactly one
// dimension -- the read is in a scope that never binds the name itself.
TEST(TypeChecker, AClosureReadOfAnOuterLocalWithNoLocalRebindStaysClean) {
    expect_clean("def o() -> int:\n"
                 "    def i() -> int:\n"
                 "        return v\n"
                 "    v: int = 1\n"
                 "    return i()\n");
}

// THE LOOP BACK-EDGE EXEMPTION, and the regression it fixes. Widening
// pre_bind_function_body to recurse into For/While bodies (closing the
// used-before-definition violations above) made the line-based ordering
// check fire across a loop's own BACK-EDGE, where mypy's flow-sensitive
// check does not -- a read textually ABOVE a same-loop-body binding can
// still execute AFTER it, on a LATER iteration. Measured against mypy 1.18.1
// and CPython 3.14 (driven with `run([1, 2, 3])`): mypy `Success`, CPython
// prints `1` then `2` at exit 0 -- this exact accumulator pattern is
// mypy-clean and CPython-clean, and was a FALSE POSITIVE this compiler
// introduced by closing the violations above, not merely an unrelated
// pre-existing gap.
TEST(TypeChecker, ALoopCarriedAccumulatorGuardedByAFlagStaysClean) {
    expect_clean("total = 0\n"
                 "def run(xs: list[int]) -> None:\n"
                 "    started = False\n"
                 "    for x in xs:\n"
                 "        if started:\n"
                 "            print(total)\n"
                 "        total = x\n"
                 "        started = True\n"
                 "run([1, 2, 3])\n");
}

// The `while` sibling of the accumulator above. Measured: mypy `Success`,
// CPython exit 0.
TEST(TypeChecker, AWhileLoopCarriedAccumulatorGuardedByAFlagStaysClean) {
    expect_clean("total = 0\n"
                 "def run(c: bool) -> None:\n"
                 "    started = False\n"
                 "    while c:\n"
                 "        if started:\n"
                 "            print(total)\n"
                 "        total = 1\n"
                 "        started = True\n"
                 "        c = False\n"
                 "run(True)\n");
}

// A `while` loop that never runs at all -- the exemption must not depend on
// the loop actually executing (this is a purely STATIC, line-based check).
// Measured: mypy `Success`, CPython exit 0 (the body, and the read inside
// it, never execute).
TEST(TypeChecker, AWhileLoopThatNeverRunsStillStaysClean) {
    expect_clean("total = 0\n"
                 "def run(c: bool) -> None:\n"
                 "    while c:\n"
                 "        print(total)\n"
                 "        total = 1\n"
                 "run(False)\n");
}

// The exemption must also cover a LATER binding that is itself a `for`
// TARGET (this file's own new arm, not just a plain Assign) -- both are
// reachable through the identical for_each_own_scope_binding walk, but a
// for-target's binding takes a DIFFERENT real-walk path (visit(For)'s
// Name-target arm) than a plain Assign, so this is not redundant with the
// accumulator test above. Measured: mypy `Success`, CPython prints `1`,
// `2` at exit 0.
TEST(TypeChecker, ALaterBindingAsAForTargetInTheSameLoopStaysClean) {
    expect_clean("total = 0\n"
                 "def run(xs: list[int]) -> None:\n"
                 "    started = False\n"
                 "    for y in xs:\n"
                 "        if started:\n"
                 "            print(total)\n"
                 "        for total in [y]:\n"
                 "            pass\n"
                 "        started = True\n"
                 "run([1, 2, 3])\n");
}

// CONTROL, THE GATE: without an ENCLOSING binding, the exemption must NOT
// fire. Measured against mypy 1.18.1 and CPython 3.14: mypy
// `Cannot determine type of "total"  [has-type]` (a real rejection, a
// DIFFERENT diagnostic from ours but still one), CPython actually runs this
// fine (exit 0, prints `1`, `2`) -- so mypy is the ONLY oracle objecting
// here, and the union rule still requires a report. Differs from
// ALoopCarriedAccumulatorGuardedByAFlagStaysClean in exactly one dimension:
// no module-level `total = 0` above the function.
TEST(TypeChecker, ALoopCarriedReadWithNoEnclosingBindingStillReports) {
    const Checked checked = check_module("def run(xs: list[int]) -> None:\n"
                                         "    started = False\n"
                                         "    for x in xs:\n"
                                         "        if started:\n"
                                         "            print(total)\n"
                                         "        total = x\n"
                                         "        started = True\n"
                                         "run([1, 2, 3])\n");

    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "NameError");
    EXPECT_EQ(error.message, "name 'total' is used before definition");
    EXPECT_EQ(error.line, 5);
}

// CONTROL, THE SPAN BOUNDARY: a read strictly BEFORE the loop even starts
// (not inside its body at all) must keep reporting -- the exemption is keyed
// on the read's own line falling WITHIN the loop's span, and a read above
// the loop entirely is outside it. Measured against mypy 1.18.1: `Success`
// (mypy is unsound here too, silent regardless), CPython
// `UnboundLocalError: cannot access local variable 'total' where it is not
// associated with a value` at exit 1 -- CPython is the only oracle
// objecting, and the union rule still requires a report. This is also the
// shape that IMPROVED as a side effect of the recursion fix: before it, this
// exact program was silently accepted (pre_bind_function_body never saw a
// `for`-body binding at all, so the read resolved outward to nothing local
// and fell through clean); it is correctly rejected now.
TEST(TypeChecker, ALoopCarriedReadStrictlyBeforeTheLoopStillReports) {
    const Checked checked = check_module("total = 0\n"
                                         "def run(xs: list[int]) -> None:\n"
                                         "    print(total)\n"
                                         "    for x in xs:\n"
                                         "        total = x\n"
                                         "run([1, 2, 3])\n");

    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "NameError");
    EXPECT_EQ(error.message, "name 'total' is used before definition");
    EXPECT_EQ(error.line, 3);
}

// CONTROL, DIFFERENT LOOPS DO NOT SHARE AN EXEMPTION: a read inside ONE
// loop's body and a binding inside a SEPARATE, sibling loop's body must
// still report -- the exemption is scoped to the SAME loop's span
// (Binding::loop_start_line/loop_end_line are the ONE loop the binding was
// found inside), not "any loop in this function". Measured against mypy
// 1.18.1: `Success` (mypy is unsound here too), CPython
// `UnboundLocalError` at exit 1 -- the read runs, unconditionally, in the
// FIRST loop, before the second loop (which does the only binding) ever
// starts.
TEST(TypeChecker, AReadInADifferentLoopFromTheBindingStillReports) {
    const Checked checked = check_module("total = 0\n"
                                         "def run(xs: list[int], ys: list[int]) -> None:\n"
                                         "    for x in xs:\n"
                                         "        print(total)\n"
                                         "    for y in ys:\n"
                                         "        total = y\n"
                                         "run([1, 2, 3], [4, 5])\n");

    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "NameError");
    EXPECT_EQ(error.message, "name 'total' is used before definition");
    EXPECT_EQ(error.line, 4);
}

// THE OUTERMOST-not-innermost CHOICE, and the false positive tagging by the
// innermost loop alone would leave: a read at an OUTER loop's own nesting
// level, guarded by a flag, checking a name that is actually assigned inside
// a FURTHER-NESTED inner loop within that same outer loop. The read and the
// write both live inside the OUTER loop's own back-edge, even though the
// write's own immediately-enclosing loop is the inner one -- tagging the
// placeholder with only the inner loop's start line left this a false
// positive (the read's line falls before the inner loop even starts).
// Measured against mypy 1.18.1 and CPython 3.14 (driven with
// `run([1, 2], [3, 4])`): mypy `Success`, CPython prints `4` at exit 0.
TEST(TypeChecker, AReadAtAnOuterLoopLevelGuardingAWriteInANestedInnerLoopStaysClean) {
    expect_clean("total = 0\n"
                 "def run(xs: list[int], ys: list[int]) -> None:\n"
                 "    started = False\n"
                 "    for x in xs:\n"
                 "        if started:\n"
                 "            print(total)\n"
                 "        for y in ys:\n"
                 "            total = y\n"
                 "        started = True\n"
                 "run([1, 2], [3, 4])\n");
}

// CONTROL: the one-line `for` suite reading its own target must stay clean
// EVEN INSIDE A FUNCTION -- this is the exact hazard order_exempt exists to
// prevent, now reachable through the placeholder-fill branch for the first
// time (a `for` target used to always take assign_name's FRESH-bind branch,
// since nothing pre-bound it; now it takes the placeholder-fill branch, which
// must thread order_exempt through rather than silently dropping it).
TEST(TypeChecker, AOneLineForSuiteInsideAFunctionReadingItsOwnTargetStaysClean) {
    expect_clean("def f() -> None:\n"
                 "    for i in range(3): print(i)\n"
                 "f()\n");
}

// The multi-line form, and the tuple-target form -- both share the same
// placeholder-fill path once pre-bound, so both need the order_exempt fix,
// not just the plain Name-target one-liner above.
TEST(TypeChecker, AMultiLineForSuiteInsideAFunctionReadingItsOwnTargetStaysClean) {
    expect_clean("def f() -> None:\n"
                 "    for i in range(3):\n"
                 "        print(i)\n"
                 "f()\n");
}

TEST(TypeChecker, ATupleForTargetInsideAFunctionIsStillUnsupportedNotACrash) {
    const Checked checked = check_module("def f() -> None:\n"
                                         "    for a, b in [(1, 2)]:\n"
                                         "        pass\n"
                                         "f()\n");

    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "NotImplementedError");
    EXPECT_EQ(error.message, "tuple targets in for loops are not supported");
}

// The one-line form, reading an element right back on the SAME line as the
// `for`: a tuple for-target's own elements are pre-bound now too (since
// pre_bind_function_body routes through for_each_bound_name, which descends
// into a TupleExpr the same way a plain Name target's enumeration does), so
// this must still take the order_exempt=true path the direct-bind block in
// visit(For) was given for exactly this reason -- ONE diagnostic (the
// existing NotImplementedError), never a SECOND, false "used before
// definition" on `a`.
TEST(TypeChecker, AOneLineTupleForSuiteInsideAFunctionIsUnsupportedNotAFalsePositive) {
    const Checked checked = check_module("def f() -> None:\n"
                                         "    for a, b in [(1, 2)]: print(a)\n"
                                         "f()\n");

    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "NotImplementedError");
    EXPECT_EQ(error.message, "tuple targets in for loops are not supported");
}

// Reproduced against the built binary before this fix:
// `def f(x: int) -> None: print(x)` reported a false "used before
// definition" on `x`, because a one-line suite's body statement sits on the
// SAME line as the `def` -- exactly the line a parameter is bound at -- so
// the ordinary ordering check's `>=` misfired. mypy accepts both the
// one-line and two-line forms; this pins the one-line form, the two-line
// form already being covered by AFullyAnnotatedFunctionIsClean.
TEST(TypeChecker, AOneLineDefReadingItsOwnParameterIsClean) {
    expect_clean("def f(x: int) -> None: print(x)\n");
}

// The SAME root cause, in the opposite direction.
// `assign_name` mistook a one-line def's parameter (declared_line == the
// body statement's own line) for pre_bind_function_body's "still-unfilled
// placeholder" and silently REBOUND over it, discarding the parameter's
// annotation -- so a wrong-typed assignment to it went unreported. This must
// report the ordinary incompatible-assignment error, exactly as the
// two-line form (AParameterAnnotationDeclaresTheNameForTheWholeBody) already
// does.
TEST(TypeChecker, AOneLineDefAssigningTheWrongTypeToItsParameterIsAnError) {
    const Checked checked = check_module("def f(x: int) -> None: x = \"s\"\n");

    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message,
              "incompatible types in assignment (expression has type \"str\", "
              "variable has type \"int\")");
}

// Pins top_level_signatures_'s entire reason to
// exist: without the cache, visit(FunctionDef) would call AnnotationResolver
// a SECOND time on the same bad annotation collect_signatures's Phase 2
// already resolved once, double-reporting it. Deleting the cache (and
// falling through to the `else` branch unconditionally) makes this test
// fail with 2 diagnostics instead of 1.
TEST(TypeChecker, ATopLevelDefsBadAnnotationIsReportedExactlyOnce) {
    const Checked checked = check_module("def f(x: Bogus) -> None:\n    pass\n");

    EXPECT_EQ(only_error(checked).code, "NameError");
}

// ANestedDefIsNotHoisted only pins the NEGATIVE case
// (a nested def is not visible before its own line). Nothing previously
// pinned that a nested def defined EARLIER is actually filled in with its
// REAL signature -- AClosureMayReadALocalAssignedAfterItsOwnDef's own
// `print(v)` never calls the nested function at all, so the rebind at
// visit(FunctionDef)'s "own name is bound before its body is checked" step
// could be deleted (leaving the Unknown placeholder unfilled, which silently
// absorbs any call) and every existing test would still pass. Calling
// `inner` with the wrong number of arguments makes a wrong signature
// observable: an unfilled Unknown placeholder would report NOTHING here.
TEST(TypeChecker, ANestedDefDefinedEarlierIsCallableWithItsRealSignature) {
    const Checked checked = check_module(
        "def outer() -> None:\n"
        "    def inner(a: int) -> int:\n"
        "        return a\n"
        "    print(inner(1, 2))\n");

    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "too many arguments for \"inner\"");
}

// Collect_signatures already checked ScopeStack::bind's
// return value for a top-level def/def collision (prior fix round); the
// parameter-binding loop below it did not, so `def f(x: int, x: str) ->
// None` silently kept only the FIRST parameter's binding instead of
// reporting the duplicate. Verified against mypy 1.18.1: `Duplicate
// argument "x" in function definition`.
TEST(TypeChecker, ADuplicateParameterNameIsReported) {
    const Checked checked = check_module("def f(x: int, x: str) -> None:\n    pass\n");

    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "duplicate argument \"x\" in function definition");
}

// The zero-parameter-method path reported its own
// "method must have at least one argument" error and returned WITHOUT ever
// resolving the return annotation, so a bad one went unreported alongside
// it. Both are real, independent errors on this input; asserting the
// diagnostic COUNT is what pins that the return annotation is resolved at
// all (a dropped resolution would silently leave this at one diagnostic).
TEST(TypeChecker, AZeroParameterMethodsBadReturnAnnotationIsStillReported) {
    const Checked checked = check_module("class C:\n    def m() -> Bogus:\n        pass\n");

    EXPECT_EQ(checked.diagnostics.size(), 2u);
    bool saw_missing_self = false;
    bool saw_bad_annotation = false;
    for (const diagnostics::Diagnostic& diagnostic : checked.diagnostics) {
        if (diagnostic.code == "TypeError" &&
            diagnostic.message == "method must have at least one argument") {
            saw_missing_self = true;
        }
        if (diagnostic.code == "NameError" && diagnostic.message == "name 'Bogus' is not defined") {
            saw_bad_annotation = true;
        }
    }
    EXPECT_TRUE(saw_missing_self);
    EXPECT_TRUE(saw_bad_annotation);
}

// ---------------------------------------------------------------------------
// Task 19: ClassDef and attribute collection.
// ---------------------------------------------------------------------------

TEST(TypeChecker, CollectsClassBodyAnnotations) {
    expect_clean("class C:\n    x: int = 5\nc = C()\ny: int = c.x\n");
}

TEST(TypeChecker, AValuelessClassBodyAnnotationIsStillAnAttribute) {
    expect_clean("class C:\n    x: int\nc = C()\ny: int = c.x\n");
}

// Verified: mypy does NOT privilege __init__. Any self.x = ... in any method
// declares the attribute.
TEST(TypeChecker, CollectsSelfAssignmentsFromAnyMethod) {
    expect_clean(
        "class C:\n"
        "    def setup(self) -> None:\n"
        "        self.x = 5\n"
        "c = C()\n"
        "y: int = c.x\n");
}

// Verified: the FIRST self.x assignment declares the type; a later
// conflicting one is an assignment error. No join, no union.
TEST(TypeChecker, TheFirstSelfAssignmentDeclaresTheAttributeType) {
    const Checked checked = check_module(
        "class C:\n"
        "    def a(self) -> None:\n"
        "        self.x = 5\n"
        "    def b(self) -> None:\n"
        "        self.x = \"s\"\n");

    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.line, 5);
}

// ---------------------------------------------------------------------------
// Forward references through self,
// a plain class-body Assign, a function-local class,
// a class-body annotation conflicting with an earlier self assignment,
// and a losing class redefinition's own body no longer
// corrupting the winner's constructor.
// ---------------------------------------------------------------------------

// Reproduced against the
// built binary before this fix: `self.b()` called from a method defined
// ABOVE `b` reported a false "C has no attribute b" -- one of the single
// most common Python shapes there is (e.g. __init__ calling a helper defined
// later in the class). declare_method used to run LAZILY, only when Phase
// 3's single pass actually reached the callee's own FunctionDef node;
// pre_collect_class_body now declares every method's signature before ANY
// of the class's own body is walked. Verified mypy-clean.
TEST(TypeChecker, AMethodMayCallAnotherMethodDefinedBelowIt) {
    expect_clean(
        "class C:\n"
        "    def a(self) -> None:\n"
        "        self.b()\n"
        "    def b(self) -> None:\n"
        "        pass\n");
}

// A method reading
// self.x where the attribute is first ASSIGNED by a method occurring BELOW
// it in the class body. pre_collect_class_body's own recursive scan over
// every method's body (collect_self_attribute_placeholders) placeholder-
// declares this before either method's body is walked for real.
TEST(TypeChecker, AMethodMayReadAnAttributeFirstAssignedByALaterMethod) {
    expect_clean(
        "class C:\n"
        "    def a(self) -> None:\n"
        "        y: int = self.x\n"
        "    def b(self) -> None:\n"
        "        self.x = 5\n");
}

// The attribute half's other source: a class-body AnnAssign appearing BELOW
// the method that reads it via self.
TEST(TypeChecker, AMethodMayReadAClassBodyAttributeDeclaredBelowIt) {
    expect_clean(
        "class C:\n"
        "    def m(self) -> None:\n"
        "        y: int = self.x\n"
        "    x: int\n");
}

// ---------------------------------------------------------------------------
// The ANNOTATED self.x form.
//
// `self.ys: list[int] = ys` -- close to universal in typed Python -- reached
// visit(AnnAssign)'s non-Name-target fallback, which resolved the annotation
// for `expected` and declared NOTHING, so every later read of the attribute
// was a false attr-defined TypeError on mypy-clean code. Two halves: this
// branch of visit(AnnAssign) (the declaration itself) and
// collect_self_attribute_placeholders' own AnnAssign arm (which is what makes
// a reader method sitting ABOVE the declaring one work). Each test below
// fails if EITHER half is removed, except where noted.
// ---------------------------------------------------------------------------

// The declared type is the ANNOTATION, not Unknown -- so the negative half
// is what stops this passing vacuously against an absorbing placeholder.
TEST(TypeChecker, AnAnnotatedSelfAssignmentDeclaresTheAttribute) {
    expect_clean(
        "class Bag:\n"
        "    def __init__(self, ys: list[int]) -> None:\n"
        "        self.ys: list[int] = ys\n"
        "    def total(self) -> int:\n"
        "        return sum(self.ys)\n");

    const Checked checked = check_module(
        "class Bag:\n"
        "    def __init__(self, ys: list[int]) -> None:\n"
        "        self.ys: list[int] = ys\n"
        "b = Bag([1])\n"
        "s: str = b.ys\n");

    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message,
              "incompatible types in assignment (expression has type \"list[int]\", "
              "variable has type \"str\")")
        << "the ANNOTATION must be the declared type, not an absorbing Unknown";
}

// The pre-pass arm on its own: the reader method sits ABOVE the declaring one, so
// nothing has walked the annotation by the time the read is typed. Only
// collect_self_attribute_placeholders' AnnAssign arm can make this clean.
TEST(TypeChecker, AMethodMayReadAnAttributeFirstAnnotatedByALaterMethod) {
    expect_clean(
        "class Bag:\n"
        "    def total(self) -> int:\n"
        "        return sum(self.ys)\n"
        "    def __init__(self, ys: list[int]) -> None:\n"
        "        self.ys: list[int] = ys\n");
}

// A VALUE-LESS annotated form declares the member too (`self.ys: list[int]`
// with no value is legal in a method body, and mypy-clean both to write and
// to read back). The declaration must not be skipped on the no-value path.
TEST(TypeChecker, AValuelessAnnotatedSelfAssignmentStillDeclaresTheAttribute) {
    expect_clean(
        "class Bag:\n"
        "    def declare(self) -> None:\n"
        "        self.ys: list[int]\n"
        "    def total(self) -> int:\n"
        "        return sum(self.ys)\n");
}

// The value is still CHECKED against the annotation, so declaring the member
// did not cost the diagnostic that makes the annotated form worth writing.
// mypy reports `List item 0 has incompatible type "str"; expected "int"` for
// exactly this program, and the per-item rule is what fires here too -- ONE
// diagnostic, not the item error plus an assignment error on top.
//
// A REGRESSION GUARD, not coverage of the declaring branch: this passed
// before that branch existed too, since the old non-Name fallback already
// resolved the annotation and ran the same value check. What it pins is that
// no later rework of the branch drops that check.
TEST(TypeChecker, AnAnnotatedSelfAssignmentStillChecksItsValue) {
    const Checked checked = check_module(
        "class Bag:\n"
        "    def __init__(self) -> None:\n"
        "        self.ys: list[int] = [\"s\"]\n");

    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "list item 0 has incompatible type \"str\"; expected \"int\"");
    EXPECT_EQ(error.line, 3);
}

// An earlier PLAIN self.x assignment IN THE SAME CLASS already declared the
// attribute. mypy 1.18.1 on exactly this program:
//
//   error: Attribute "x" already defined on line 3  [no-redef]
//   error: Incompatible types in assignment (expression has type "str",
//          variable has type "int")  [assignment]
//   note: Revealed type is "builtins.int"          (a third method's self.x)
//
// So the EARLIER declaration stays the attribute's type and the annotated
// statement's VALUE is what gets checked against it -- the annotation itself
// is ignored. We report the assignment error only (the no-redef is a
// deliberate missed error, see the branch's own comment). Both diagnostics
// land at the annotation's own line, which is where we report too.
TEST(TypeChecker, AnAnnotationConflictingWithAnEarlierSelfAssignmentIsReported) {
    const Checked checked = check_module(
        "class Bag:\n"
        "    def a(self) -> None:\n"
        "        self.x = 5\n"
        "    def b(self) -> None:\n"
        "        self.x: str = \"s\"\n");

    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message,
              "incompatible types in assignment (expression has type \"str\", "
              "variable has type \"int\")")
        << "the EARLIER declaration is the variable; the annotation is ignored";
    EXPECT_EQ(error.line, 5) << "reported at the annotation, which is where mypy reports too";
}

// The ORDER THAT IS ACTUALLY MYPY-CLEAN (verified 1.18.1, unlike the
// annotation-second shape above): annotate first, then plainly assign a
// subtype. int into a float attribute must not report.
//
// What this covers is the PLAIN sibling path, NOT the annotated branch's
// existing-declaration arm: the annotation at line 3 is the first occurrence
// of "n", so it takes the OwnPlaceholder path (its own placeholder was
// declared at its own line) and the arm below is never reached at all. The
// float-vs-int comparison this pins is assign_attribute's ordinary
// read-then-compare one, on line 5. Left in place because that comparison
// has been written backwards before, but it is not coverage of the
// annotated form's own rules -- AClassBodyAnnotationSurvivesAMethod
// Annotation and its neighbours below are.
//
// The clean half ALONE proved nothing, and this is what the second half is
// for: with the annotated `self.x` branch absent altogether (its state at
// 6192fab, before this work), the annotation declared NOTHING, "n" became
// plain int from the line-5 assignment, and `f: float = b.n` was clean for
// an unrelated reason. `i: int = b.n` discriminates -- it is SILENT against
// the 6192fab binary and reports `expression has type "float"` now, mypy's
// own message for the same program (`error: Incompatible types in assignment
// (expression has type "float", variable has type "int")`, line 8), which is
// only possible once the ANNOTATED type is what the attribute carries.
TEST(TypeChecker, APlainSelfAssignmentOfASubtypeAfterAnAnnotationKeepsTheAnnotatedType) {
    expect_clean(
        "class Bag:\n"
        "    def a(self) -> None:\n"
        "        self.n: float = 1.5\n"
        "    def b(self) -> None:\n"
        "        self.n = 5\n"
        "b = Bag()\n"
        "f: float = b.n\n");

    const Checked checked = check_module(
        "class Bag:\n"
        "    def a(self) -> None:\n"
        "        self.n: float = 1.5\n"
        "    def b(self) -> None:\n"
        "        self.n = 5\n"
        "b = Bag()\n"
        "f: float = b.n\n"
        "i: int = b.n\n");

    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message,
              "incompatible types in assignment (expression has type \"float\", "
              "variable has type \"int\")")
        << "the attribute carries the ANNOTATED float, not the assignment's int";
    EXPECT_EQ(error.line, 8);
}

// ---------------------------------------------------------------------------
// THE TWO RULES for an annotated `self.x` whose attribute is ALREADY
// declared. mypy 1.18.1 has two different answers here depending on WHICH
// CLASS the earlier declaration lives on, and a single rule -- whichever way
// round its comparison was written -- was a false TypeError on one of them.
// Thirteen mypy-clean programs of the narrowing shape reported a false
// TypeError before this fix; the four tests below are the ones that would
// have caught it.
// ---------------------------------------------------------------------------

// INHERITED, narrowing. mypy: `Success: no issues found`, and
// `reveal_type(self.v)` is "builtins.int" in EVERY Child method, not only the
// annotating one -- so the subclass annotation really does install a
// narrower per-class type. The `self.v + 1` here is what pins the install:
// keeping the base's `object` instead makes this mypy-clean line a false
// "unsupported operand types" TypeError.
TEST(TypeChecker, ASubclassAnnotationMayNarrowAnInheritedAttribute) {
    expect_clean(
        "class Base:\n"
        "    def __init__(self) -> None:\n"
        "        self.v: object = 1\n"
        "class Child(Base):\n"
        "    def m(self) -> None:\n"
        "        self.v: int = 1\n"
        "    def use(self) -> int:\n"
        "        return self.v + 1\n");
}

// INHERITED, and the narrowing is CONFINED to the subclass: mypy accepts
// `self.v = "s"` in Base (declared object there) on the very same program,
// and reports it in Child. Both halves matter -- declaring the override onto
// the CURRENT class rather than onto the class that owns the declaration is
// what keeps Base's own `object` intact.
TEST(TypeChecker, ASubclassNarrowingDoesNotChangeTheBaseClassDeclaration) {
    expect_clean(
        "class Base:\n"
        "    def __init__(self) -> None:\n"
        "        self.v: object = 1\n"
        "    def w(self) -> None:\n"
        "        self.v = \"s\"\n"
        "class Child(Base):\n"
        "    def m(self) -> None:\n"
        "        self.v: int = 1\n");

    const Checked checked = check_module(
        "class Base:\n"
        "    def __init__(self) -> None:\n"
        "        self.v: object = 1\n"
        "class Child(Base):\n"
        "    def m(self) -> None:\n"
        "        self.v: int = 1\n"
        "    def w(self) -> None:\n"
        "        self.v = \"s\"\n");

    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.line, 8) << "the narrowed int is Child's declared type for writes too";
}

// INHERITED, WIDENING -- the direction mypy DOES reject: `error: Incompatible
// types in assignment (expression has type "object", base class "Base"
// defined the type as "int")`. The comparison must stay, and must stay this
// way round: the annotation is the expression, the inherited declaration the
// variable. Deleting the comparison outright makes this test fail.
TEST(TypeChecker, ASubclassAnnotationWideningAnInheritedAttributeIsReported) {
    const Checked checked = check_module(
        "class Base:\n"
        "    def __init__(self) -> None:\n"
        "        self.v: int = 1\n"
        "class Child(Base):\n"
        "    def m(self) -> None:\n"
        "        self.v: object = 1\n");

    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message,
              "incompatible types in assignment (expression has type \"object\", "
              "variable has type \"int\")");
    EXPECT_EQ(error.line, 6);
}

// SAME CLASS: a class-body annotation is NOT replaced by a method
// annotation. mypy on the first program: `Success`, with
// `reveal_type(self.n)` still "builtins.object" in every other method and in
// `Bag().n` -- so the `self.n = "s"` here is clean, and installing the
// method's `int` would make it a false TypeError. On the second program mypy
// reports `expression has type "str", variable has type "int"`, its own
// message for a VALUE checked against the class-body type, verbatim -- and
// note it is the value, not the annotation, that is checked: `self.n: int =
// "s"` under a class-body `n: object` is mypy-CLEAN.
TEST(TypeChecker, AClassBodyAnnotationSurvivesAMethodAnnotation) {
    expect_clean(
        "class Bag:\n"
        "    n: object\n"
        "    def set(self) -> None:\n"
        "        self.n: int = 7\n"
        "    def w(self) -> None:\n"
        "        self.n = \"s\"\n");

    expect_clean(
        "class Bag:\n"
        "    n: object\n"
        "    def set(self) -> None:\n"
        "        self.n: int = \"s\"\n");

    const Checked checked = check_module(
        "class Bag:\n"
        "    n: int\n"
        "    def set(self) -> None:\n"
        "        self.n: str = \"s\"\n");

    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message,
              "incompatible types in assignment (expression has type \"str\", "
              "variable has type \"int\")");
    EXPECT_EQ(error.line, 4);
}

// THE GUARD, and the reason self_attribute_receiver_type resolves `self`
// through ScopeStack rather than trusting the spelling: a NESTED def whose
// own first parameter is named `self` but TYPED AS SOMETHING ELSE must NOT
// declare a member on the enclosing class. mypy reports its own errors for
// this program (a non-self attribute declaration, and `"int" has no
// attribute "q"`), so the read below must still be attr-defined here -- a
// clean result would mean the nested def had silently declared "q" on Bag.
//
// A NEGATIVE GUARD, not coverage of the declaring branch: it passes with
// that branch removed too (nothing declared "q" then either). What it pins
// is that the branch did not widen the guard. It pins only the
// DIFFERENT-type case -- `def inner(self: Bag)` inside a Bag method DOES
// pass the guard and declares onto Bag, verified against the built binary;
// see self_attribute_receiver_type's own comment for that measured missed
// error.
TEST(TypeChecker, AnAnnotatedAttributeOnAShadowedSelfDeclaresNothing) {
    const Checked checked = check_module(
        "class Bag:\n"
        "    def outer(self) -> None:\n"
        "        def inner(self: int) -> None:\n"
        "            self.q: int = 1\n"
        "    def read(self) -> int:\n"
        "        return self.q\n");

    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "\"Bag\" has no attribute \"q\"");
    EXPECT_EQ(error.line, 6);
}

// The sibling of the test above, and the case the TYPE check alone could not
// catch: a nested `def inner(self: Bag)` inside a Bag method binds `self` to
// exactly Class("Bag"), so the type check passes and `self.q = 1` there used
// to declare "q" on Bag -- making the later read come out clean. Telling that
// case apart needs the syntactic question a type check cannot ask: is this
// `self` a METHOD's own first parameter? mypy reports three errors on this
// program (attr-defined at the store, attr-defined at the read, and
// `Returning Any` on the read's own return); the two attr-defined ones are
// what this checker matches.
TEST(TypeChecker, ANestedDefsSelfDoesNotDeclareOntoTheEnclosingClass) {
    const Checked checked = check_module(
        "class Bag:\n"
        "    def m(self) -> None:\n"
        "        def inner(self: Bag) -> None:\n"
        "            self.q = 1\n"
        "        inner(self)\n"
        "    def read(self) -> int:\n"
        "        return self.q\n");

    ASSERT_EQ(checked.diagnostics.size(), 2u);
    EXPECT_EQ(checked.diagnostics[0].code, "TypeError");
    EXPECT_EQ(checked.diagnostics[0].message, "\"Bag\" has no attribute \"q\"");
    EXPECT_EQ(checked.diagnostics[0].line, 4) << "the store, inside the nested def";
    EXPECT_EQ(checked.diagnostics[1].code, "TypeError");
    EXPECT_EQ(checked.diagnostics[1].message, "\"Bag\" has no attribute \"q\"");
    EXPECT_EQ(checked.diagnostics[1].line, 7) << "the read, from the other method";
}

// The SAME rule through the guard's OTHER call site: `self.x: T = ...` in
// visit(AnnAssign)'s Attribute-target branch, which shares
// self_attribute_receiver_type verbatim with assign_attribute above. Tested
// separately because the test above exercises only the plain-Assign site, so
// a change that fixed one and not the other would pass it.
//
// Only ONE diagnostic, and the asymmetry is pre-existing rather than
// introduced here: an annotated attribute STORE never types its target, so
// the store itself stays silent (exactly as
// AnAnnotatedAttributeOnANonSelfReceiverDeclaresNothing already records for
// a non-self receiver) and the READ is what reports. mypy reports four
// errors on this program -- `Type cannot be declared in assignment to
// non-self attribute` and attr-defined at the store, `Returning Any` and
// attr-defined at the read -- so three are missed. What matters is that the
// read is no longer CLEAN: before this change the annotated store declared
// "q" on Bag and cythonpp said nothing at all.
TEST(TypeChecker, ANestedDefsAnnotatedSelfAttributeDoesNotDeclareOntoTheEnclosingClass) {
    const Checked checked = check_module(
        "class Bag:\n"
        "    def m(self) -> None:\n"
        "        def inner(self: Bag) -> None:\n"
        "            self.q: int = 1\n"
        "        inner(self)\n"
        "    def read(self) -> int:\n"
        "        return self.q\n");

    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "\"Bag\" has no attribute \"q\"");
    EXPECT_EQ(error.line, 7) << "the READ reports; the annotated store itself stays silent";
}

// UNCHANGED, and this is the guard that matters: a real method's own `self`
// still declares. Every existing self.x test covers this, but state it here
// too so a regression shows up next to the change that could cause it.
TEST(TypeChecker, AMethodsOwnSelfStillDeclaresOntoItsClass) {
    expect_clean("class Bag:\n"
                 "    def __init__(self) -> None:\n"
                 "        self.n = 0\n"
                 "    def read(self) -> int:\n"
                 "        return self.n\n");
}

// A nested def inside a method must still be able to READ an attribute the
// enclosing class really declares -- the change is about DECLARING, not
// reading. Written UNQUOTED deliberately: the `"Bag"` string forward
// reference the same program could use is a NotImplementedError in this
// grammar (measured: "string forward references are not supported"), so the
// quoted spelling would test the deferral rather than this rule. Both
// spellings are mypy-clean.
TEST(TypeChecker, ANestedDefCanStillReadARealAttribute) {
    expect_clean("class Bag:\n"
                 "    def __init__(self) -> None:\n"
                 "        self.n = 0\n"
                 "    def m(self) -> int:\n"
                 "        def inner(b: Bag) -> int:\n"
                 "            return b.n\n"
                 "        return inner(self)\n");
}

// THE COUNTERWEIGHT to the rule above, and the reason Binding::method_self is
// a fact about the BINDING rather than about the innermost function. A nested
// def with NO `self` parameter of its own CAPTURES the enclosing method's,
// and mypy attributes the store to that method's self: measured 2026-09-11
// against mypy 1.18.1, this exact program is "Success: no issues found",
// including the `self.q` read from the OTHER method, and CPython runs it.
//
// A guard phrased as "the immediately enclosing function must itself be a
// method" was implemented and measured: it reported
// `"Bag" has no attribute "q"` at BOTH lines here -- two false TypeErrors on
// a program both oracles accept. This test is what fails if that phrasing is
// ever reintroduced, and the whole existing suite passed with it in place.
TEST(TypeChecker, AClosureCapturingAMethodsSelfStillDeclaresOntoItsClass) {
    expect_clean("class Bag:\n"
                 "    def m(self) -> None:\n"
                 "        def inner() -> None:\n"
                 "            self.q = 1\n"
                 "        inner()\n"
                 "    def read(self) -> int:\n"
                 "        return self.q\n");
}

// The same rule at depth: mypy does not care HOW MANY function scopes the
// captured `self` is closed over (measured: "Success" for this program too),
// so neither may this guard. Separate from the single-closure case above
// because a rule that walked exactly one scope outward would pass that one
// and fail this one.
TEST(TypeChecker, AClosureTwoDeepCapturingAMethodsSelfStillDeclares) {
    expect_clean("class Bag:\n"
                 "    def m(self) -> None:\n"
                 "        def a() -> None:\n"
                 "            def b() -> None:\n"
                 "                self.q = 1\n"
                 "            b()\n"
                 "        a()\n"
                 "    def read(self) -> int:\n"
                 "        return self.q\n");
}

// An annotated attribute store on a NON-self receiver is unchanged: the
// annotation is resolved for `expected`, nothing is declared, and the TARGET
// is never typed -- so the store itself is silent (mypy reports two errors
// for it: a non-self type declaration, and attr-defined). That silence is a
// pre-existing MISSED error on this path, deliberately left alone; what this
// test pins is that the store declared NOTHING, evidenced by the read below
// it still being attr-defined. If the self.x branch ever stopped checking
// the receiver, the read would come back clean instead.
//
// A NEGATIVE GUARD, like the one above: it passes with the declaring branch
// removed too, since nothing declared "z" on Other before it either.
TEST(TypeChecker, AnAnnotatedAttributeOnANonSelfReceiverDeclaresNothing) {
    const Checked checked = check_module(
        "class Other:\n"
        "    y: int\n"
        "def f(o: Other) -> None:\n"
        "    o.z: int = 3\n"
        "def g(o: Other) -> int:\n"
        "    return o.z\n");

    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "\"Other\" has no attribute \"z\"");
    EXPECT_EQ(error.line, 6) << "the READ reports; the annotated store itself stays silent";
}

// Defect 4. A method's first parameter need not be spelled `self` -- mypy
// keys on which BINDING the receiver resolves to, not the literal name.
// Measured 2026-09-12: mypy --strict is Success and CPython prints 1, while
// this checker reported TWO errors (one for the store, one for the read).
TEST(TypeChecker, AMethodsFirstParameterNeedNotBeNamedSelf) {
    expect_clean("class Bag:\n"
                 "    def read(self) -> int:\n"
                 "        return self.q\n"
                 "\n"
                 "    def m(this) -> None:\n"
                 "        this.q = 1\n");
}

// Defect 2, B1: the reader sits ABOVE the closure that declares. This is the
// direction that was broken; the sibling case (reader BELOW, see
// AClosureCapturingAMethodsSelfStillDeclaresOntoItsClass above) was already
// clean, which is why it survived the tests written for the closure case.
TEST(TypeChecker, AClosureAssignedAttributeIsVisibleToAReaderAbove) {
    expect_clean("class Bag:\n"
                 "    def read(self) -> int:\n"
                 "        return self.q\n"
                 "\n"
                 "    def m(self) -> None:\n"
                 "        def inner() -> None:\n"
                 "            self.q = 1\n"
                 "        inner()\n");
}

// B3: depth is irrelevant -- mypy attributes the store through ANY number of
// capturing closures, and so must the pre-pass that makes a reader above see
// it.
TEST(TypeChecker, AnAttributeAssignedTwoClosuresDeepIsStillDeclared) {
    expect_clean("class Bag:\n"
                 "    def read(self) -> int:\n"
                 "        return self.q\n"
                 "\n"
                 "    def m(self) -> None:\n"
                 "        def mid() -> None:\n"
                 "            def inner() -> None:\n"
                 "                self.q = 1\n"
                 "            inner()\n"
                 "        mid()\n");
}

// B6: the ANNOTATED form through a closure. The guard has a second call site
// in visit(AnnAssign), so a fix applied to only one site passes a test set
// that exercises only the other.
TEST(TypeChecker, AnAnnotatedClosureAssignedAttributeIsDeclared) {
    expect_clean("class Bag:\n"
                 "    def read(self) -> int:\n"
                 "        return self.q\n"
                 "\n"
                 "    def m(self) -> None:\n"
                 "        def inner() -> None:\n"
                 "            self.q: int = 1\n"
                 "        inner()\n");
}

// B17: control-flow nesting around the closure is irrelevant.
TEST(TypeChecker, AClosureInsideAForStillDeclares) {
    expect_clean("class Bag:\n"
                 "    def read(self) -> int:\n"
                 "        return self.q\n"
                 "\n"
                 "    def m(self, xs: list[int]) -> None:\n"
                 "        for x in xs:\n"
                 "            def inner() -> None:\n"
                 "                self.q = 1\n"
                 "            inner()\n");
}

// B10 combined with the closure: the captured name is `this`, not `self`.
TEST(TypeChecker, AClosureCapturingANonSelfNamedReceiverDeclares) {
    expect_clean("class Bag:\n"
                 "    def read(self) -> int:\n"
                 "        return self.q\n"
                 "\n"
                 "    def m(this) -> None:\n"
                 "        def inner() -> None:\n"
                 "            this.q = 1\n"
                 "        inner()\n");
}

// Controls that must keep reporting. Each differs from the clean shapes above
// in exactly one dimension.

// B4: the nested def REBINDS the name, so the store is not the method's.
// Differs from AClosureAssignedAttributeIsVisibleToAReaderAbove ONLY in
// whether `inner` takes a parameter named `self`.
//
// A version of this test with no read() above (asserting only the store's
// own diagnostic via `only_error`) would NOT discriminate the shadow guard
// at all -- it passes identically whether `shadows` is computed correctly or
// forced false, because the shadowing parameter is typed `int` (a builtin),
// so `self.q = 1` there is independently blocked by
// self_attribute_receiver_type's own TYPE check (int != Bag) regardless of
// what the pre-pass's shadow decision was. That store-side diagnostic is a
// red herring for what this test needs to cover. Adding a `read()` above
// (the same shape B5 uses) makes the shadow guard observable: if the
// pre-pass wrongly descends into `inner` anyway, it placeholder-declares "q"
// as Unknown and the read's diagnostic silently disappears. Verified by
// temporarily forcing the descent unconditionally: only the store's
// (guard-independent) NotImplementedError survived, confirming this shape
// genuinely exercises the guard.
//
// Measured against the built binary 2026-09-12: TWO diagnostics -- the
// read() above (never declared, since the shadowed `self` never binds "q"
// onto Bag) and the store itself (typed `int`, so it takes the ordinary,
// non-self attribute path and reports the builtin-member NotImplementedError
// rather than attr-defined). mypy reports three errors on this program
// (`Returning Any`, attr-defined at the read, attr-defined at the store);
// cythonpp's own wording differs at the store (NotImplementedError, since
// this compiler does not model builtin-type attribute access at all) but
// still reports something at both positions -- not silent acceptance.
TEST(TypeChecker, ANestedDefShadowingSelfDoesNotDeclareOntoTheClass) {
    const Checked checked = check_module("class Bag:\n"
                                         "    def read(self) -> int:\n"
                                         "        return self.q\n"
                                         "    def m(self) -> None:\n"
                                         "        def inner(self: int) -> None:\n"
                                         "            self.q = 1\n");

    ASSERT_EQ(checked.diagnostics.size(), 2u);
    EXPECT_EQ(checked.diagnostics[0].code, "TypeError");
    EXPECT_EQ(checked.diagnostics[0].message, "\"Bag\" has no attribute \"q\"");
    EXPECT_EQ(checked.diagnostics[0].line, 3) << "the read, from read()";
    EXPECT_EQ(checked.diagnostics[1].code, "NotImplementedError");
    EXPECT_EQ(checked.diagnostics[1].message, "methods on builtin types are not supported");
    EXPECT_EQ(checked.diagnostics[1].line, 6) << "the store, independent of the shadow guard";
}

// B5: THE ANNOTATION IS IRRELEVANT. Shadowing blocks declaration even when
// the shadowing parameter is annotated as the enclosing class ITSELF. This
// control differs from B4 only in that annotation, which is precisely the
// dimension a wrong guard would key on.
//
// Measured against the built binary 2026-09-12: this shape draws TWO
// diagnostics, not one -- the read() above (never declared, since the
// shadowed `self` never binds "q" onto Bag) AND the store itself (which,
// unlike B4, resolves to Class("Bag") so it takes the ordinary self-shaped
// attribute path and reports attr-defined rather than the builtin-member
// error). only_error() would fail here for the wrong reason, so both are
// pinned instead, matching the existing
// ANestedDefsSelfDoesNotDeclareOntoTheEnclosingClass regression test's shape.
TEST(TypeChecker, ANestedDefShadowingSelfAsTheClassStillDoesNotDeclare) {
    const Checked checked = check_module("class Bag:\n"
                                         "    def read(self) -> int:\n"
                                         "        return self.q\n"
                                         "\n"
                                         "    def m(self) -> None:\n"
                                         "        def inner(self: Bag) -> None:\n"
                                         "            self.q = 1\n");

    ASSERT_EQ(checked.diagnostics.size(), 2u);
    EXPECT_EQ(checked.diagnostics[0].code, "TypeError");
    EXPECT_EQ(checked.diagnostics[0].message, "\"Bag\" has no attribute \"q\"");
    EXPECT_EQ(checked.diagnostics[0].line, 3) << "the read, from read()";
    EXPECT_EQ(checked.diagnostics[1].code, "TypeError");
    EXPECT_EQ(checked.diagnostics[1].message, "\"Bag\" has no attribute \"q\"");
    EXPECT_EQ(checked.diagnostics[1].line, 7) << "the store, inside the shadowing nested def";
}

// B7: a nested CLASS's `self` is the inner class's, not Bag's.
//
// This test's source (like every other check_module fixture here) never
// calls `m()`, so CPython never executes the `class Inner` body at all and
// has no opinion on it -- measured, CPython exits 0 on this exact source,
// the exact "a CPython verdict on a function body is worthless unless the
// probe CALLS the function" trap CLAUDE.md warns about. Only mypy rejects
// it (`"Bag" has no attribute "q"` plus `Returning Any`). Driving it
// (`Bag().m()` then reading) DOES make CPython raise the same AttributeError
// mypy predicts, but that is a fact about a DIFFERENT, driven program, not
// this one.
//
// Measured: only ONE diagnostic from cythonpp. `self.q = 1` inside Inner.go
// declares "q" on Inner (go's own `self` is a real method_self binding, just
// for the wrong class), so only the read() above -- which asks about Bag --
// ever reports.
TEST(TypeChecker, ANestedClassMethodDoesNotDeclareOntoTheOuterClass) {
    const Checked checked = check_module("class Bag:\n"
                                         "    def read(self) -> int:\n"
                                         "        return self.q\n"
                                         "\n"
                                         "    def m(self) -> None:\n"
                                         "        class Inner:\n"
                                         "            def go(self) -> None:\n"
                                         "                self.q = 1\n");

    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "\"Bag\" has no attribute \"q\"");
    EXPECT_EQ(error.line, 3);
}

// B8: attribution is via the BINDING, not the receiver's static type -- a
// differently-named parameter annotated as Bag does not declare.
//
// Measured: TWO diagnostics, the same shape as B5 -- the read() above and the
// store through `other`, which types as Bag but is not a method_self binding.
TEST(TypeChecker, AClosureAssigningThroughANonSelfParameterDoesNotDeclare) {
    const Checked checked = check_module("class Bag:\n"
                                         "    def read(self) -> int:\n"
                                         "        return self.q\n"
                                         "\n"
                                         "    def m(self) -> None:\n"
                                         "        def inner(other: Bag) -> None:\n"
                                         "            other.q = 1\n");

    ASSERT_EQ(checked.diagnostics.size(), 2u);
    EXPECT_EQ(checked.diagnostics[0].code, "TypeError");
    EXPECT_EQ(checked.diagnostics[0].message, "\"Bag\" has no attribute \"q\"");
    EXPECT_EQ(checked.diagnostics[0].line, 3) << "the read, from read()";
    EXPECT_EQ(checked.diagnostics[1].code, "TypeError");
    EXPECT_EQ(checked.diagnostics[1].message, "\"Bag\" has no attribute \"q\"");
    EXPECT_EQ(checked.diagnostics[1].line, 7) << "the store, through the non-self parameter";
}

// B15: paths are compared SYNTACTICALLY, so an alias does not follow. Same
// rule NarrowingMap documents, and for the same reason.
//
// Measured: TWO diagnostics, the same shape as B5/B8.
TEST(TypeChecker, AnAliasOfSelfDoesNotDeclare) {
    const Checked checked = check_module("class Bag:\n"
                                         "    def read(self) -> int:\n"
                                         "        return self.q\n"
                                         "\n"
                                         "    def m(self) -> None:\n"
                                         "        alias = self\n"
                                         "        alias.q = 1\n");

    ASSERT_EQ(checked.diagnostics.size(), 2u);
    EXPECT_EQ(checked.diagnostics[0].code, "TypeError");
    EXPECT_EQ(checked.diagnostics[0].message, "\"Bag\" has no attribute \"q\"");
    EXPECT_EQ(checked.diagnostics[0].line, 3) << "the read, from read()";
    EXPECT_EQ(checked.diagnostics[1].code, "TypeError");
    EXPECT_EQ(checked.diagnostics[1].message, "\"Bag\" has no attribute \"q\"");
    EXPECT_EQ(checked.diagnostics[1].line, 7) << "the store, through the alias";
}

// Python makes a name local to a function's scope by ANY assignment to it
// there, not only by it appearing as a parameter: `def inner(): self =
// Bag(); self.q = 1` rebinds `self` to a local of `inner` exactly as a
// shadowing parameter would. A shadow check that inspects only parameters
// misses this and lets collect_self_attribute_placeholders descend into
// `inner` anyway, placeholder-declaring "q" as Unknown -- and Unknown is
// absorbing, so it silences BOTH the store and every reader, reporting
// NOTHING AT ALL. Measured 2026-09-12 against mypy 1.18.1 and CPython 3.14:
// mypy reports attr-defined at BOTH the store and the read (plus a
// `Returning Any`), and CPython raises `AttributeError: 'Bag' object has no
// attribute 'q'` -- both oracles reject this program, so a parameter-only
// shadow check is strictly worse than never descending into a closure at
// all.
TEST(TypeChecker, ARebindingAssignmentInAClosureDoesNotDeclareOntoTheClass) {
    const Checked checked = check_module("class Bag:\n"
                                         "    def read(self) -> int:\n"
                                         "        return self.q\n"
                                         "    def m(self) -> None:\n"
                                         "        def inner() -> None:\n"
                                         "            self = Bag()\n"
                                         "            self.q = 1\n"
                                         "        inner()\n");

    ASSERT_EQ(checked.diagnostics.size(), 2u);
    EXPECT_EQ(checked.diagnostics[0].code, "TypeError");
    EXPECT_EQ(checked.diagnostics[0].message, "\"Bag\" has no attribute \"q\"");
    EXPECT_EQ(checked.diagnostics[0].line, 3) << "the read, from read()";
    EXPECT_EQ(checked.diagnostics[1].code, "TypeError");
    EXPECT_EQ(checked.diagnostics[1].message, "\"Bag\" has no attribute \"q\"");
    EXPECT_EQ(checked.diagnostics[1].line, 7) << "the store, after inner() rebinds self";
}

// The `for`-target variant of the same rebinding: `for self in [1, 2]:`
// inside the closure makes `self` local to `inner` exactly as `self =
// Bag()` does. Measured: mypy reports attr-defined at both the store (typed
// "int", since the loop variable's declared type comes from the iterable)
// and the read (plus `Returning Any`); CPython raises `AttributeError: 'int'
// object has no attribute 'q' and no __dict__ for setting new attributes`.
// Here the store itself was NEVER silent -- assigning an attribute on a
// builtin-typed receiver already draws its own NotImplementedError,
// independent of this guard -- but the READ above it used to come back
// clean, because the pre-pass still wrongly placeholder-declared "q".
TEST(TypeChecker, ARebindingForTargetInAClosureDoesNotDeclareOntoTheClass) {
    const Checked checked = check_module("class Bag:\n"
                                         "    def read(self) -> int:\n"
                                         "        return self.q\n"
                                         "    def m(self) -> None:\n"
                                         "        def inner() -> None:\n"
                                         "            for self in [1, 2]:\n"
                                         "                self.q = 1\n"
                                         "        inner()\n");

    ASSERT_EQ(checked.diagnostics.size(), 2u);
    EXPECT_EQ(checked.diagnostics[0].code, "TypeError");
    EXPECT_EQ(checked.diagnostics[0].message, "\"Bag\" has no attribute \"q\"");
    EXPECT_EQ(checked.diagnostics[0].line, 3)
        << "the read, from read() -- which the store's own diagnostic below does not cover";
    EXPECT_EQ(checked.diagnostics[1].code, "NotImplementedError");
    EXPECT_EQ(checked.diagnostics[1].message, "methods on builtin types are not supported");
    EXPECT_EQ(checked.diagnostics[1].line, 7) << "the store, through the for-target's int type";
}

// self_attribute_receiver_type resolving the receiver by its own spelling
// (rather than the literal "self") has a TYPE-PRECISION signature, not a
// silence signature -- a test that only asserts "no diagnostic" cannot pin
// it, because the self-attribute pre-pass's own placeholder is Unknown, and
// Unknown is absorbing enough to keep every clean test and every "does it
// still report" control passing whether or not the receiver is resolved
// correctly. The question that DOES distinguish the two is what type the
// attribute actually ends up with: `read()`'s declared return type is "str",
// but the attribute was only ever assigned an int, so the REAL type has to
// have propagated for this to fire. Measured against mypy 1.18.1:
// `Incompatible return value type (got "int", expected "str")`.
TEST(TypeChecker, ANonSelfNamedReceiversAttributeKeepsItsRealTypeNotUnknown) {
    const Checked checked = check_module("class Bag:\n"
                                         "    def m(this) -> None:\n"
                                         "        this.q = 1\n"
                                         "    def read(self) -> str:\n"
                                         "        return self.q\n");

    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "incompatible return value type (got \"int\", expected \"str\")");
    EXPECT_EQ(error.line, 5);
}

// The closure sibling of the test above: the attribute is assigned through a
// CAPTURED self inside a closure, and must still carry its real type (int),
// not Unknown, once the closure's own store is reached. Measured against
// mypy 1.18.1: same message as above.
TEST(TypeChecker, AClosureAssignedAttributeKeepsItsRealTypeNotUnknown) {
    const Checked checked = check_module("class Bag:\n"
                                         "    def m(self) -> None:\n"
                                         "        def inner() -> None:\n"
                                         "            self.q = 1\n"
                                         "        inner()\n"
                                         "    def read(self) -> str:\n"
                                         "        return self.q\n");

    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "incompatible return value type (got \"int\", expected \"str\")");
    EXPECT_EQ(error.line, 7);
}

// Rebinding the receiver is not only an Assign/AnnAssign/for-target -- a
// nested `class self: ...` binds "self" in the ENCLOSING scope (inner's own
// scope) exactly as an assignment would, and this is a distinct rebinding
// form from the one Assign/AnnAssign/For alone can see: a nested ClassDef or
// FunctionDef's OWN NAME rebinds the enclosing scope, while everything
// bound INSIDE that nested def/class's own body is a different scope and
// must keep being ignored. Conflating "does the nested def/class's body
// rebind the name" with "does the nested def/class's own name rebind it" is
// the trap this test guards against.
//
// Measured against mypy 1.18.1 (driven with `b = Bag(); b.m(); print(b.read())`
// at module level): THREE errors -- `Returning Any`, attr-defined at the read
// (line 2) and attr-defined at the store (line 6, `"type[self]" has no
// attribute "q"` -- mypy treats the shadowing `self` as the class object
// itself). CPython raises `AttributeError: 'Bag' object has no attribute
// 'q'`. Before this fix cythonpp was COMPLETELY SILENT (0 diagnostics) --
// the pre-pass didn't see `class self:` as a shadow at all and wrongly
// placeholder-declared "q" on Bag.
//
// After this fix: only ONE diagnostic, the read. The store itself is NOT
// caught, for a separate, structural reason this fix does not (and should
// not try to) touch: ScopeStack deliberately never binds a class's own name
// (see "Class names are deliberately NOT bound into ScopeStack" elsewhere in
// this file, load-bearing for two unrelated checks), so
// self_attribute_receiver_type's own real-walk resolution of "self" still
// cannot see that `class self:` shadowed it, and treats the store as a
// legitimate new declaration onto Bag. That gap is recorded separately in
// CLAUDE.md rather than fixed here -- what this test pins is that the
// pre-pass no longer manufactures the placeholder that hid the READ above
// it, which is the exact silent-acceptance shape a nested def/class's own
// name must be checked for.
TEST(TypeChecker, AClassDefRebindingTheReceiverInAClosureDoesNotDeclare) {
    const Checked checked = check_module("class Bag:\n"
                                         "    def read(self) -> int:\n"
                                         "        return self.q\n"
                                         "    def m(self) -> None:\n"
                                         "        def inner() -> None:\n"
                                         "            class self:\n"
                                         "                pass\n"
                                         "            self.q = 1\n"
                                         "        inner()\n");

    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "\"Bag\" has no attribute \"q\"");
    EXPECT_EQ(error.line, 3);
}

// The FunctionDef sibling of the test above: `def self(): ...` inside the
// closure also binds "self" in the enclosing (inner's) scope. Unlike a
// nested ClassDef, an ordinary nested `def` DOES get bound into ScopeStack
// at its lexical position (ClassDef names deliberately do not -- see the
// test above), so self_attribute_receiver_type's own real-walk resolution
// CAN see this particular shadow: it resolves "self" to the shadowing def's
// own Callable type, which is not Class("Bag"), so the store falls through
// to the ordinary (non-self) attribute path -- the SAME builtin-member-style
// NotImplementedError this compiler already reports for any attribute
// access on a Callable receiver, independent of this fix. So this shape is
// fully closed by this fix: both the read (now correctly attr-defined,
// where before it was silently swallowed by the wrongly-placeholder-declared
// attribute) and the store (already reported before this fix, unaffected)
// come back with a diagnostic.
//
// Measured against mypy 1.18.1: THREE errors -- `Returning Any`, attr-defined
// at the read (line 2), and attr-defined at the store (line 6,
// `"Callable[[], None]" has no attribute "q"`). CPython raises the same
// `AttributeError: 'Bag' object has no attribute 'q'` at the read (since it
// is reached first).
TEST(TypeChecker, AFunctionDefRebindingTheReceiverInAClosureDoesNotDeclare) {
    const Checked checked = check_module("class Bag:\n"
                                         "    def read(self) -> int:\n"
                                         "        return self.q\n"
                                         "    def m(self) -> None:\n"
                                         "        def inner() -> None:\n"
                                         "            def self() -> None:\n"
                                         "                pass\n"
                                         "            self.q = 1\n"
                                         "        inner()\n");

    ASSERT_EQ(checked.diagnostics.size(), 2u);
    EXPECT_EQ(checked.diagnostics[0].code, "TypeError");
    EXPECT_EQ(checked.diagnostics[0].message, "\"Bag\" has no attribute \"q\"");
    EXPECT_EQ(checked.diagnostics[0].line, 3) << "the read, from read()";
    EXPECT_EQ(checked.diagnostics[1].code, "NotImplementedError");
    EXPECT_EQ(checked.diagnostics[1].message, "methods on builtin types are not supported");
    EXPECT_EQ(checked.diagnostics[1].line, 8) << "the store, through the shadowing def's Callable type";
}

// THE METHOD-LEVEL (no closure at all) sibling of the two tests above:
// `class self: pass` written DIRECTLY in `m`'s own body, with no nested def
// in between. Until 2026-09-13 this was a genuine UNION-RULE VIOLATION, not
// merely a missed refinement like the closure variant's residual store gap --
// receiver_rebound_in_own_scope was only ever consulted when
// collect_self_attribute_placeholders decided whether to DESCEND INTO a
// nested def, never for a rebinding at the SAME level as the self.x = ...
// statement being scanned, so this exact shape was COMPLETELY SILENT (0
// diagnostics) where mypy reports `Name "self" already defined on line 4
// [no-redef]` and CPython raises `AttributeError: 'Bag' object has no
// attribute 'q'` when driven. Fixed by two independent changes landing
// together: (1) the same shadow check now also runs before scanning a
// method's OWN body, not only before descending into a nested one, so the
// attribute placeholder is no longer wrongly declared; (2) visit(ClassDef)
// now reports a redefinition directly when a function-local class's name
// collides with an existing same-scope binding (here, the `self` parameter)
// -- a check with no precedent before this fix, since a class's own name is
// never otherwise compared against anything. TWO diagnostics here, not one:
// this compiler does not need to match mypy's count, only refuse to stay
// silent on a program mypy rejects.
TEST(TypeChecker, AClassDefRebindingTheReceiverAtMethodLevelReportsBoth) {
    const Checked checked = check_module("class Bag:\n"
                                         "    def read(self) -> int:\n"
                                         "        return self.q\n"
                                         "    def m(self) -> None:\n"
                                         "        class self:\n"
                                         "            pass\n"
                                         "        self.q = 1\n");

    ASSERT_EQ(checked.diagnostics.size(), 2u);
    EXPECT_EQ(checked.diagnostics[0].code, "TypeError");
    EXPECT_EQ(checked.diagnostics[0].message, "\"Bag\" has no attribute \"q\"");
    EXPECT_EQ(checked.diagnostics[0].line, 3) << "the read, from read() above m()";
    EXPECT_EQ(checked.diagnostics[1].code, "TypeError");
    EXPECT_EQ(checked.diagnostics[1].message, "name \"self\" already defined on line 4");
    EXPECT_EQ(checked.diagnostics[1].line, 5) << "the class statement itself, colliding with the parameter";
}

// mypy's redefinition rule is POSITION-INDEPENDENT: a reader placed BELOW
// `m` (rather than above it, as in the test above) still collides, because
// `m`'s own real per-statement walk (unaffected by this fix, since it is a
// completely separate code path from the placeholder pre-pass) declares "q"
// through the ordinary self.x = ... handling before the reader below it is
// ever reached -- so only the redefinition itself is left to report, and it
// still is, because receiver_rebound_in_own_scope's check does not depend on
// there being a reader at all. Measured against mypy 1.18.1 and CPython 3.14
// (driven with `b = Bag(); b.m(); print(b.read())`): mypy `no-redef` at the
// class statement, CPython prints `1` then raises nothing -- wait, CPython
// actually raises nothing here at all (`AttributeError` was the closure
// variant's story; here the ordinary store legitimately runs before the
// class statement rebinds the name, so `b.read()` sees a real attribute) --
// the class statement is REACHED regardless, but the ATTRIBUTE was already
// set through the actual parameter before the shadowing class statement runs.
// mypy still rejects (a static check, unaffected by runtime order), so the
// union rule requires cythonpp to report too.
TEST(TypeChecker, AClassDefRebindingTheReceiverReportsRegardlessOfReaderPosition) {
    const Checked checked = check_module("class Bag:\n"
                                         "    def m(self) -> None:\n"
                                         "        class self:\n"
                                         "            pass\n"
                                         "        self.q = 1\n"
                                         "    def read(self) -> int:\n"
                                         "        return self.q\n");

    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "name \"self\" already defined on line 2");
    EXPECT_EQ(error.line, 3);
}

// mypy's rule needs no reader at all: the collision is between the class
// statement and the parameter, not between anything and an attribute read.
TEST(TypeChecker, AClassDefRebindingTheReceiverReportsWithNoReaderAtAll) {
    const Checked checked = check_module("class Bag:\n"
                                         "    def m(self) -> None:\n"
                                         "        class self:\n"
                                         "            pass\n"
                                         "        self.q = 1\n");

    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "name \"self\" already defined on line 2");
    EXPECT_EQ(error.line, 3);
}

// And POSITION-independent the other way too: the store can sit ABOVE the
// rebinding class statement. mypy still reports (its redefinition check does
// not care which comes first textually), and CPython actually runs this
// clean (`self.q = 1` executes through the real parameter before the class
// statement ever reassigns the local), so this is a case where ONLY mypy
// objects -- exactly the shape the union rule still requires a report for.
TEST(TypeChecker, AClassDefRebindingAfterTheStoreStillReports) {
    const Checked checked = check_module("class Bag:\n"
                                         "    def m(self) -> None:\n"
                                         "        self.q = 1\n"
                                         "        class self:\n"
                                         "            pass\n");

    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "name \"self\" already defined on line 2");
    EXPECT_EQ(error.line, 4);
}

// CONTROL: rebinding a DIFFERENT name must not suppress the attribute or
// trip the new redefinition check -- differs from the test above in exactly
// one dimension, the rebound name.
TEST(TypeChecker, AClassDefRebindingADifferentNameStaysClean) {
    expect_clean("class Bag:\n"
                 "    def read(self) -> int:\n"
                 "        return self.q\n"
                 "    def m(self) -> None:\n"
                 "        class other:\n"
                 "            pass\n"
                 "        self.q = 1\n");
}

// CONTROL: a rebinding in a DIFFERENT method must not affect `m`/`read` at
// all -- receiver_rebound_in_own_scope and the new ClassDef redefinition
// check are both scoped to the ONE method's own body being scanned. Differs
// from AClassDefRebindingTheReceiverAtMethodLevelReportsBoth in exactly one
// dimension, which method the class statement sits in.
TEST(TypeChecker, AClassDefRebindingInADifferentMethodDoesNotAffectAnother) {
    const Checked checked = check_module("class Bag:\n"
                                         "    def read(self) -> int:\n"
                                         "        return self.q\n"
                                         "    def m(self) -> None:\n"
                                         "        self.q = 1\n"
                                         "    def other(self) -> None:\n"
                                         "        class self:\n"
                                         "            pass\n");

    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "name \"self\" already defined on line 6")
        << "other()'s OWN parameter, on other()'s own def line -- not m()'s";
    EXPECT_EQ(error.line, 7) << "other()'s own collision, unrelated to m()/read()";
}

// The Assign sibling of the tests above, AT METHOD LEVEL rather than inside a
// closure: `self = Bag()` directly in `m`'s own body. UNLIKE the ClassDef
// shape, mypy is CLEAN on this one (an ordinary variable rebinding, not a
// `[no-redef]`-eligible definition), so there is no redefinition diagnostic
// to gain here -- ONLY the attribute placeholder fix applies, and this shape
// is mypy-clean/CPython-AttributeError, so per the corpus harness's own gate
// it belongs in a unit test, not the labelled corpus. Measured against mypy
// 1.18.1: `Success`. CPython (driven): `AttributeError: 'Bag' object has no
// attribute 'q'`.
TEST(TypeChecker, AnAssignRebindingTheReceiverAtMethodLevelDoesNotDeclare) {
    const Checked checked = check_module("class Bag:\n"
                                         "    def read(self) -> int:\n"
                                         "        return self.q\n"
                                         "    def m(self) -> None:\n"
                                         "        self = Bag()\n"
                                         "        self.q = 1\n");

    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "\"Bag\" has no attribute \"q\"");
    EXPECT_EQ(error.line, 3);
}

// The `for` target sibling, AT METHOD LEVEL: `for self in [Bag(), Bag()]:`
// directly in `m`'s own body. Also mypy-clean (`Success`), so a unit test
// rather than a corpus sample, for the same reason as the Assign sibling
// above. CPython (driven): `AttributeError: 'Bag' object has no attribute
// 'q'`.
TEST(TypeChecker, AForTargetRebindingTheReceiverAtMethodLevelDoesNotDeclare) {
    const Checked checked = check_module("class Bag:\n"
                                         "    def read(self) -> int:\n"
                                         "        return self.q\n"
                                         "    def m(self) -> None:\n"
                                         "        for self in [Bag(), Bag()]:\n"
                                         "            pass\n"
                                         "        self.q = 1\n");

    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "\"Bag\" has no attribute \"q\"");
    EXPECT_EQ(error.line, 3);
}

// A `this`-named receiver (not spelled "self") rebound the same way,
// confirming the fix is keyed on the BINDING (a method's own first
// parameter), not the literal spelling "self" -- matches
// self_attribute_receiver_type's own naming independence elsewhere in this
// file.
TEST(TypeChecker, AClassDefRebindingAThisNamedReceiverReportsBoth) {
    const Checked checked = check_module("class Bag:\n"
                                         "    def read(this) -> int:\n"
                                         "        return this.q\n"
                                         "    def m(this) -> None:\n"
                                         "        class this:\n"
                                         "            pass\n"
                                         "        this.q = 1\n");

    ASSERT_EQ(checked.diagnostics.size(), 2u);
    EXPECT_EQ(checked.diagnostics[0].code, "TypeError");
    EXPECT_EQ(checked.diagnostics[0].message, "\"Bag\" has no attribute \"q\"");
    EXPECT_EQ(checked.diagnostics[1].code, "TypeError");
    EXPECT_EQ(checked.diagnostics[1].message, "name \"this\" already defined on line 4");
}

// The other half of receiver_rebound_in_own_scope's rule -- never descending
// INTO a nested def/class's own BODY -- has no test of its own anywhere
// above: every existing shape puts the rebinding directly in the nested
// def's own scope (a parameter, an Assign, a for-target, or the def/class's
// own name), never one level deeper. A rebinding buried inside a FURTHER
// nested def must NOT be treated as shadowing the outer closure's receiver,
// because it rebinds only ITS OWN scope, not `inner`'s. Measured 2026-09-12
// against mypy 1.18.1 and CPython 3.14 (driven with `b = Bag(); b.m();
// print(b.read())`): mypy `Success`, CPython prints `1` at exit 0 -- `self`
// inside `deeper` is a local of `deeper` alone, so `self.q = 1` in `inner`
// still refers to the method's own receiver and legitimately declares "q".
TEST(TypeChecker, ARebindingInAFurtherNestedDefDoesNotShadowTheOuterClosure) {
    expect_clean("class Bag:\n"
                 "    def read(self) -> int:\n"
                 "        return self.q\n"
                 "    def m(self) -> None:\n"
                 "        def inner() -> None:\n"
                 "            def deeper() -> None:\n"
                 "                self = 5\n"
                 "            self.q = 1\n"
                 "        inner()\n");
}

// The same rule with an extra layer of control flow around the buried
// rebinding: a further nested def's own body is still a different scope
// even when the rebinding inside it sits under an `if`. Measured 2026-09-12
// against mypy 1.18.1 and CPython 3.14 (driven the same way): mypy
// `Success`, CPython prints `1` at exit 0.
TEST(TypeChecker, ARebindingInAnIfInsideAFurtherNestedDefDoesNotShadow) {
    expect_clean("class Bag:\n"
                 "    def read(self) -> int:\n"
                 "        return self.q\n"
                 "    def m(self) -> None:\n"
                 "        def inner() -> None:\n"
                 "            def deeper() -> None:\n"
                 "                if True:\n"
                 "                    self = 5\n"
                 "            self.q = 1\n"
                 "        inner()\n");
}

// The ClassDef sibling: a rebinding of the receiver name inside a nested
// class's own BODY (a class-body assignment, not the class's own name) is
// that class's own attribute, in that class's own scope -- it must not
// shadow the outer closure's receiver either. Measured 2026-09-12 against
// mypy 1.18.1 and CPython 3.14 (driven the same way): mypy `Success`,
// CPython prints `1` at exit 0 -- `Holder.self` is a class attribute of
// `Holder`, entirely unrelated to the `self` `inner` still resolves outward
// to declare "q" through.
TEST(TypeChecker, ARebindingInsideANestedClassBodyDoesNotShadow) {
    expect_clean("class Bag:\n"
                 "    def read(self) -> int:\n"
                 "        return self.q\n"
                 "    def m(self) -> None:\n"
                 "        def inner() -> None:\n"
                 "            class Holder:\n"
                 "                self = 1\n"
                 "            self.q = 1\n"
                 "        inner()\n");
}

// receiver_rebound_in_own_scope's AnnAssign arm: `self: Bag = Bag()` rebinds
// exactly as the plain `self = Bag()` form does, and is a genuinely separate
// arm from it in the scan (a distinct AST node, ast::AnnAssign rather than
// ast::Assign). Measured against mypy 1.18.1 and CPython 3.14 (driven with
// `b = Bag(); b.m(); print(b.read())` at module level): mypy reports THREE
// errors (`Returning Any`, attr-defined at the read on line 3, attr-defined
// at the store on line 7); CPython raises `AttributeError: 'Bag' object has
// no attribute 'q'` at the read, exit 1 -- both oracles reject.
TEST(TypeChecker, AnAnnotatedRebindingInAClosureDoesNotDeclareOntoTheClass) {
    const Checked checked = check_module("class Bag:\n"
                                         "    def read(self) -> int:\n"
                                         "        return self.q\n"
                                         "    def m(self) -> None:\n"
                                         "        def inner() -> None:\n"
                                         "            self: Bag = Bag()\n"
                                         "            self.q = 1\n"
                                         "        inner()\n");

    ASSERT_EQ(checked.diagnostics.size(), 2u);
    EXPECT_EQ(checked.diagnostics[0].code, "TypeError");
    EXPECT_EQ(checked.diagnostics[0].message, "\"Bag\" has no attribute \"q\"");
    EXPECT_EQ(checked.diagnostics[0].line, 3) << "the read, from read()";
    EXPECT_EQ(checked.diagnostics[1].code, "TypeError");
    EXPECT_EQ(checked.diagnostics[1].message, "\"Bag\" has no attribute \"q\"");
    EXPECT_EQ(checked.diagnostics[1].line, 7) << "the store, after inner() rebinds self via AnnAssign";
}

// receiver_rebound_in_own_scope's If-body recursion arm: a rebinding sitting
// under an `if` is still in `inner`'s own scope (an If body is not a new
// scope), so it must be found by recursing into the branch rather than only
// scanning `inner`'s own top-level statements. Measured against mypy 1.18.1
// and CPython 3.14 (driven the same way): mypy reports THREE errors
// (`Returning Any`, attr-defined at the read on line 3, attr-defined at the
// store on line 8); CPython raises the same AttributeError at the read,
// exit 1 -- both oracles reject.
TEST(TypeChecker, ARebindingInsideAnIfBodyInAClosureDoesNotDeclare) {
    const Checked checked = check_module("class Bag:\n"
                                         "    def read(self) -> int:\n"
                                         "        return self.q\n"
                                         "    def m(self) -> None:\n"
                                         "        def inner() -> None:\n"
                                         "            if True:\n"
                                         "                self = Bag()\n"
                                         "            self.q = 1\n"
                                         "        inner()\n");

    ASSERT_EQ(checked.diagnostics.size(), 2u);
    EXPECT_EQ(checked.diagnostics[0].code, "TypeError");
    EXPECT_EQ(checked.diagnostics[0].message, "\"Bag\" has no attribute \"q\"");
    EXPECT_EQ(checked.diagnostics[0].line, 3) << "the read, from read()";
    EXPECT_EQ(checked.diagnostics[1].code, "TypeError");
    EXPECT_EQ(checked.diagnostics[1].message, "\"Bag\" has no attribute \"q\"");
    EXPECT_EQ(checked.diagnostics[1].line, 8) << "the store, after the if body rebinds self";
}

// receiver_rebound_in_own_scope's If-orelse recursion arm: the same
// rebinding, now sitting in the `else` arm rather than the `if` body -- a
// distinct branch of the same node, and a distinct recursive call in the
// scan. Measured against mypy 1.18.1 and CPython 3.14 (driven the same way):
// mypy reports THREE errors (`Returning Any`, attr-defined at the read on
// line 3, attr-defined at the store on line 10); CPython raises the same
// AttributeError at the read, exit 1 -- both oracles reject.
TEST(TypeChecker, ARebindingInsideAnIfsElseArmInAClosureDoesNotDeclare) {
    const Checked checked = check_module("class Bag:\n"
                                         "    def read(self) -> int:\n"
                                         "        return self.q\n"
                                         "    def m(self) -> None:\n"
                                         "        def inner() -> None:\n"
                                         "            if False:\n"
                                         "                pass\n"
                                         "            else:\n"
                                         "                self = Bag()\n"
                                         "            self.q = 1\n"
                                         "        inner()\n");

    ASSERT_EQ(checked.diagnostics.size(), 2u);
    EXPECT_EQ(checked.diagnostics[0].code, "TypeError");
    EXPECT_EQ(checked.diagnostics[0].message, "\"Bag\" has no attribute \"q\"");
    EXPECT_EQ(checked.diagnostics[0].line, 3) << "the read, from read()";
    EXPECT_EQ(checked.diagnostics[1].code, "TypeError");
    EXPECT_EQ(checked.diagnostics[1].message, "\"Bag\" has no attribute \"q\"");
    EXPECT_EQ(checked.diagnostics[1].line, 10) << "the store, after the if's else arm rebinds self";
}

// receiver_rebound_in_own_scope's While-body recursion arm, the same
// question as the If-body arm for a different compound statement. Measured
// against mypy 1.18.1 and CPython 3.14 (driven the same way): mypy reports
// THREE errors (`Returning Any`, attr-defined at the read on line 3,
// attr-defined at the store on line 9); CPython raises the same
// AttributeError at the read, exit 1 -- both oracles reject.
TEST(TypeChecker, ARebindingInsideAWhileBodyInAClosureDoesNotDeclare) {
    const Checked checked = check_module("class Bag:\n"
                                         "    def read(self) -> int:\n"
                                         "        return self.q\n"
                                         "    def m(self) -> None:\n"
                                         "        def inner() -> None:\n"
                                         "            while True:\n"
                                         "                self = Bag()\n"
                                         "                break\n"
                                         "            self.q = 1\n"
                                         "        inner()\n");

    ASSERT_EQ(checked.diagnostics.size(), 2u);
    EXPECT_EQ(checked.diagnostics[0].code, "TypeError");
    EXPECT_EQ(checked.diagnostics[0].message, "\"Bag\" has no attribute \"q\"");
    EXPECT_EQ(checked.diagnostics[0].line, 3) << "the read, from read()";
    EXPECT_EQ(checked.diagnostics[1].code, "TypeError");
    EXPECT_EQ(checked.diagnostics[1].message, "\"Bag\" has no attribute \"q\"");
    EXPECT_EQ(checked.diagnostics[1].line, 9) << "the store, after the while body rebinds self";
}

// receiver_rebound_in_own_scope's While-orelse recursion arm: a `while`'s
// `else` runs when the loop condition becomes false without a `break`, and
// is a distinct branch (and a distinct recursive call) from the body.
// Measured against mypy 1.18.1 and CPython 3.14 (driven the same way): mypy
// reports THREE errors (`Returning Any`, attr-defined at the read on line 3,
// attr-defined at the store on line 10); CPython raises the same
// AttributeError at the read, exit 1 -- both oracles reject.
TEST(TypeChecker, ARebindingInsideAWhilesElseArmInAClosureDoesNotDeclare) {
    const Checked checked = check_module("class Bag:\n"
                                         "    def read(self) -> int:\n"
                                         "        return self.q\n"
                                         "    def m(self) -> None:\n"
                                         "        def inner() -> None:\n"
                                         "            while False:\n"
                                         "                pass\n"
                                         "            else:\n"
                                         "                self = Bag()\n"
                                         "            self.q = 1\n"
                                         "        inner()\n");

    ASSERT_EQ(checked.diagnostics.size(), 2u);
    EXPECT_EQ(checked.diagnostics[0].code, "TypeError");
    EXPECT_EQ(checked.diagnostics[0].message, "\"Bag\" has no attribute \"q\"");
    EXPECT_EQ(checked.diagnostics[0].line, 3) << "the read, from read()";
    EXPECT_EQ(checked.diagnostics[1].code, "TypeError");
    EXPECT_EQ(checked.diagnostics[1].message, "\"Bag\" has no attribute \"q\"");
    EXPECT_EQ(checked.diagnostics[1].line, 10) << "the store, after the while's else arm rebinds self";
}

// receiver_rebound_in_own_scope's For-body recursion arm, the same question
// as the If/While body arms for a `for` loop's own body. Measured against
// mypy 1.18.1 and CPython 3.14 (driven the same way): mypy reports THREE
// errors (`Returning Any`, attr-defined at the read on line 3, attr-defined
// at the store on line 8); CPython raises the same AttributeError at the
// read, exit 1 -- both oracles reject.
TEST(TypeChecker, ARebindingInsideAForBodyInAClosureDoesNotDeclare) {
    const Checked checked = check_module("class Bag:\n"
                                         "    def read(self) -> int:\n"
                                         "        return self.q\n"
                                         "    def m(self) -> None:\n"
                                         "        def inner() -> None:\n"
                                         "            for i in [1]:\n"
                                         "                self = Bag()\n"
                                         "            self.q = 1\n"
                                         "        inner()\n");

    ASSERT_EQ(checked.diagnostics.size(), 2u);
    EXPECT_EQ(checked.diagnostics[0].code, "TypeError");
    EXPECT_EQ(checked.diagnostics[0].message, "\"Bag\" has no attribute \"q\"");
    EXPECT_EQ(checked.diagnostics[0].line, 3) << "the read, from read()";
    EXPECT_EQ(checked.diagnostics[1].code, "TypeError");
    EXPECT_EQ(checked.diagnostics[1].message, "\"Bag\" has no attribute \"q\"");
    EXPECT_EQ(checked.diagnostics[1].line, 8) << "the store, after the for body rebinds self";
}

// receiver_rebound_in_own_scope's For-orelse recursion arm: a `for`'s `else`
// runs when the loop completes without a `break`, and is a distinct branch
// (and a distinct recursive call) from the body, exactly as While's own
// orelse arm is. Measured against mypy 1.18.1 and CPython 3.14 (driven the
// same way): mypy reports THREE errors (`Returning Any`, attr-defined at the
// read on line 3, attr-defined at the store on line 10); CPython raises the
// same AttributeError at the read, exit 1 -- both oracles reject.
TEST(TypeChecker, ARebindingInsideAForsElseArmInAClosureDoesNotDeclare) {
    const Checked checked = check_module("class Bag:\n"
                                         "    def read(self) -> int:\n"
                                         "        return self.q\n"
                                         "    def m(self) -> None:\n"
                                         "        def inner() -> None:\n"
                                         "            for i in [1]:\n"
                                         "                pass\n"
                                         "            else:\n"
                                         "                self = Bag()\n"
                                         "            self.q = 1\n"
                                         "        inner()\n");

    ASSERT_EQ(checked.diagnostics.size(), 2u);
    EXPECT_EQ(checked.diagnostics[0].code, "TypeError");
    EXPECT_EQ(checked.diagnostics[0].message, "\"Bag\" has no attribute \"q\"");
    EXPECT_EQ(checked.diagnostics[0].line, 3) << "the read, from read()";
    EXPECT_EQ(checked.diagnostics[1].code, "TypeError");
    EXPECT_EQ(checked.diagnostics[1].message, "\"Bag\" has no attribute \"q\"");
    EXPECT_EQ(checked.diagnostics[1].line, 10) << "the store, after the for's else arm rebinds self";
}

// Reproduced against the built binary
// before this fix: `class D: x = 5` then `d.x` reported a false "D has no
// attribute x" -- only the AnnAssign path ever called declare_member; a bare
// class-body constant (mypy-clean) did not. Handled in assign_to's own
// Name-target branch now, gated on is_new_definition so the member's type is
// the FIRST assignment's, matching every other "first assignment is sticky"
// rule in this file.
//
// The positive half ALONE is VACUOUS -- deleting the
// declare_member call this test is meant to pin still leaves it passing,
// because pre_collect_class_body's own pre-pass placeholder-declares "x" as
// Unknown regardless, type_of_attribute returns that Unknown for `d.x`, and
// the AnnAssign compare below is guarded on BOTH sides being non-Unknown, so
// `y: int = d.x` is clean either way. Strengthened with the negative half:
// this only passes once the REAL inferred type (int, not Unknown) is filled
// in by assign_to's own declare_member call.
TEST(TypeChecker, CollectsAPlainClassBodyAssignment) {
    expect_clean("class D:\n    x = 5\nd = D()\ny: int = d.x\n");

    const Checked checked = check_module("class D:\n    x = 5\nd = D()\ny: str = d.x\n");
    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message,
              "incompatible types in assignment (expression has type \"int\", "
              "variable has type \"str\")");
}

// The plain class-body
// Assign path (in assign_to) had NO has_value()/line guard at all, unlike
// its AnnAssign sibling and assign_attribute's own self.x path
// -- so a plain class-body Assign appearing BELOW a method that already
// assigned self.x silently RE-TYPED the attribute with ZERO diagnostics.
// Reproduced against the built binary before this fix.
//
// The comparison here once ran the WRONG WAY
// ROUND, which is why this test originally asserted the OPPOSITE polarity
// from mypy's. Verified against real mypy 1.18.1 on this exact program:
//   c3.py:3: error: Incompatible types in assignment (expression has type
//   "int", variable has type "str")  [assignment]
// -- a class-body assignment's inferred type is the attribute's DECLARED
// type, and the earlier self.x's value is what gets checked against it, so
// the message below is now mypy's own wording verbatim. Only the LINE still
// differs (mypy reports at the self.x assignment, line 3; we report at the
// class-body statement, line 4), the residual recorded earlier and now
// does not close.
TEST(TypeChecker, APlainClassBodyAssignmentConflictingWithAnEarlierSelfAssignmentIsReported) {
    const Checked checked = check_module(
        "class C:\n"
        "    def m(self) -> None:\n"
        "        self.x = 5\n"
        "    x = \"s\"\n");

    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message,
              "incompatible types in assignment (expression has type \"int\", "
              "variable has type \"str\")");
}

// The false-positive half, and the whole reason the
// direction above had to be swapped. Both of these are mypy-clean --
// verified against real mypy 1.18.1, including reveal_type of the resulting
// attribute, which is the CLASS-BODY assignment's type in both cases
// ("builtins.int" for the first, "builtins.float" for the second) -- and
// the backwards comparison made both a false TypeError, the hard
// invariant this project exists to protect.
TEST(TypeChecker, AClassBodyAssignmentWideningAnEarlierSelfAssignmentIsClean) {
    // bool widens into the declared int.
    expect_clean(
        "class C:\n"
        "    def m(self) -> None:\n"
        "        self.x = True\n"
        "    x = 5\n");
    // int widens into the declared float, via the numeric tower.
    expect_clean(
        "class C:\n"
        "    def m(self) -> None:\n"
        "        self.x = 5\n"
        "    x = 1.5\n");
}

// The SECOND arm of the same defect, in visit(AnnAssign).
// pre_collect_class_body's annotation
// sub-pass only handles a DIRECT class-body AnnAssign, so one NESTED inside
// an `if` within the class body still reaches visit(AnnAssign)'s own
// already_member comparison -- which ran the same wrong way round. Verified
// mypy-clean against real mypy 1.18.1 (and reveal_type of the attribute is
// "builtins.int", the ANNOTATION's type, confirming the annotation is the
// declared type here exactly as it is for a direct one).
TEST(TypeChecker, AClassBodyAnnotationNestedInAnIfWideningAnEarlierSelfAssignmentIsClean) {
    expect_clean(
        "FLAG = True\n"
        "class C:\n"
        "    def m(self) -> None:\n"
        "        self.x = True\n"
        "    if FLAG:\n"
        "        x: int\n");
}

// ---------------------------------------------------------------------------
// THE OTHER HALF of the same two-rule split, at the two CLASS-BODY sites.
// The four tests above cover the SAME-CLASS rule (an earlier declaration on
// this very class); these cover the INHERITED one, which the two class-body
// sites got wrong in both directions until they stopped gating on the
// chain-walking member_type and started gating on own_member_type. Every
// program below was run through mypy 1.18.1, and every clean one reported a
// false TypeError against the built binary before the fix (identical output
// at 6192fab, so a pre-existing defect rather than a regression).
//
// Note the same-class and inherited rules point in OPPOSITE directions here,
// which is exactly why one comparison could not serve both: same-class, the
// class-body statement wins and the earlier declaration is the expression
// checked against it; inherited, the class-body statement is still what wins
// but it is the EXPRESSION checked against the base's declaration.
// ---------------------------------------------------------------------------

// INHERITED, narrowing, the ANNOTATION form. mypy: `Success: no issues
// found`, with `reveal_type(self.v)` "builtins.int" in Child and
// "builtins.object" in Base. `self.v + 1` pins the install onto Child:
// leaving the base's `object` in place makes that mypy-clean line a false
// "unsupported operand types". The three-level chain pins that each class
// reads as its OWN declaration (bool / int / object, per mypy).
TEST(TypeChecker, AClassBodyAnnotationMayNarrowAnInheritedAttribute) {
    expect_clean(
        "class Base:\n"
        "    v: object\n"
        "class Child(Base):\n"
        "    v: int\n"
        "    def use(self) -> int:\n"
        "        return self.v + 1\n");

    expect_clean(
        "class Base:\n"
        "    v: object\n"
        "class Mid(Base):\n"
        "    v: int\n"
        "class Leaf(Mid):\n"
        "    v: bool\n"
        "    def use(self) -> int:\n"
        "        return self.v + 1\n");

    // A user-class hierarchy, not just the numeric tower and `object`.
    expect_clean(
        "class Animal:\n"
        "    pass\n"
        "class Dog(Animal):\n"
        "    pass\n"
        "class Base:\n"
        "    pet: Animal\n"
        "class Child(Base):\n"
        "    pet: Dog\n");
}

// INHERITED, narrowing, and CONFINED to the subclass -- the install must land
// on the current class and leave the base's own declaration alone. mypy
// accepts `self.v = "s"` in Base on this very program (v is object there) and
// reports it in Child (v is int there).
TEST(TypeChecker, AClassBodyNarrowingDoesNotChangeTheBaseClassDeclaration) {
    expect_clean(
        "class Base:\n"
        "    v: object\n"
        "    def w(self) -> None:\n"
        "        self.v = \"s\"\n"
        "class Child(Base):\n"
        "    v: int\n");

    const Checked checked = check_module(
        "class Base:\n"
        "    v: object\n"
        "class Child(Base):\n"
        "    v: int\n"
        "    def w(self) -> None:\n"
        "        self.v = \"s\"\n");

    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.line, 6) << "the narrowed int is Child's declared type for writes too";
}

// INHERITED, WIDENING, the ANNOTATION form -- the direction mypy DOES reject:
// `error: Incompatible types in assignment (expression has type "object",
// base class "Base" defined the type as "int")`. This one was SILENT before
// the fix (the same-class direction happens to hold for it, so nothing
// reported), which is why the clean tests above alone would not have pinned
// the split. The arguments are the way round mypy prints them: the
// annotation is the expression, the inherited declaration the variable.
TEST(TypeChecker, AClassBodyAnnotationWideningAnInheritedAttributeIsReported) {
    const Checked checked = check_module(
        "class Base:\n"
        "    v: int\n"
        "class Child(Base):\n"
        "    v: object\n");

    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message,
              "incompatible types in assignment (expression has type \"object\", "
              "variable has type \"int\")");
    EXPECT_EQ(error.line, 4);
}

// INHERITED, narrowing, the PLAIN ASSIGN form. mypy: `Success`, with
// `reveal_type(self.v)` "builtins.int" in Child -- so the INFERRED type of a
// class-body assignment overrides an inherited annotation just as an explicit
// one does. Both the arithmetic and the later `self.v = 7` pin the install:
// with the base's `object` left in place the first is a false "unsupported
// operand types" and the second a false incompatible assignment.
TEST(TypeChecker, AClassBodyAssignmentMayNarrowAnInheritedAttribute) {
    expect_clean(
        "class Base:\n"
        "    v: object\n"
        "class Child(Base):\n"
        "    v = 1\n"
        "    def use(self) -> int:\n"
        "        return self.v + 1\n"
        "    def w(self) -> None:\n"
        "        self.v = 7\n");
}

// INHERITED, WIDENING, the PLAIN ASSIGN form. mypy: `error: Incompatible
// types in assignment (expression has type "str", base class "Base" defined
// the type as "int")`. This one DID report before the fix, but with the two
// arguments the wrong way round (`expression has type "int", variable has
// type "str"`) -- so what this pins is the direction, not merely that
// something is reported.
TEST(TypeChecker, AClassBodyAssignmentWideningAnInheritedAttributeIsReported) {
    const Checked checked = check_module(
        "class Base:\n"
        "    v: int\n"
        "class Child(Base):\n"
        "    v = \"s\"\n");

    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message,
              "incompatible types in assignment (expression has type \"str\", "
              "variable has type \"int\")");
    EXPECT_EQ(error.line, 4);
}

// own_member_type does no canonicalisation, and a nested and a function-local
// class are keyed differently from a module-level one ("Outer.Inner" and a
// synthetic isolated name respectively) -- so the narrowing must be probed
// under both keyings, not just the flat one. Both mypy-clean, both a false
// TypeError before the fix.
TEST(TypeChecker, AClassBodyNarrowingWorksForNestedAndFunctionLocalClasses) {
    expect_clean(
        "class Base:\n"
        "    v: object\n"
        "class Outer:\n"
        "    class Inner(Base):\n"
        "        v: int\n"
        "        def use(self) -> int:\n"
        "            return self.v + 1\n");

    expect_clean(
        "class Base:\n"
        "    v: object\n"
        "def f() -> int:\n"
        "    class Local(Base):\n"
        "        v = 1\n"
        "        def use(self) -> int:\n"
        "            return self.v + 1\n"
        "    return Local().use()\n");
}

// A ClassDef lexically inside a `def` is never seen
// by collect_classes' Phase-1 walk (it only recurses into module- and
// class-level bodies), so it had no ClassTable entry at all by the time
// Phase 3 reached it -- every self.attr inside was a false attr-defined
// TypeError. It is now declared, under an isolated qualified name, exactly
// when Phase 3's walk reaches it.
//
// An earlier version declared a NON-colliding local
// class under its own BARE name, which had no collision detection of its
// own, so a SECOND same-named local class declared in a DIFFERENT function
// silently overwrote the first one's ClassTable entry. That was fixed by
// ALWAYS isolating under a synthetic name -- and, having done so, rewrote
// this test to assert a NameError on `Local()`, since bare-name constructor
// dispatch had been left scope-blind.
//
// That NameError is a FALSE positive. This exact
// program is clean under real mypy 1.18.1 (`mypy --strict`: "Success: no
// issues found in 1 source file"), so that traded a narrow false
// attr-defined for a BROAD false NameError on EVERY function-local class
// construction -- strictly worse, and not covered by "a missed error beats a
// false one", since the outcome was itself a false diagnostic. The isolated
// ClassTable key stays (it is what closes the overwrite and the leak); what
// is added is a SCOPE-LIMITED alias from the bare name to it, live for
// exactly the enclosing function's body. So the expect_clean this test
// carried before that isolation is restored, and it is once again the only
// assertion that pins bare-name construction of a local class working at
// all.
TEST(TypeChecker, AFunctionLocalClassIsConstructibleByItsBareNameInsideItsOwnFunction) {
    expect_clean(
        "def make() -> None:\n"
        "    class Local:\n"
        "        def __init__(self) -> None:\n"
        "            self.x = 5\n"
        "    v = Local()\n"
        "    y: int = v.x\n");
}

// Two DIFFERENT
// functions each declare their own local class under the identical bare
// name "L". Before the isolation fix, both computed the same bare "L" qualified
// name (declare() has no collision detection of its own), so the SECOND
// function's own `L()` call resolved to the FIRST function's class -- not
// merely "no longer a constructor call", but the WRONG one -- and a member
// access the first class genuinely lacks (`w.b`) was a FALSE attr-defined
// TypeError.
//
// That was closed by making `L()` unresolvable
// altogether, and this test was written to assert the resulting NameError --
// which is itself a false positive (`mypy --strict` on this exact program:
// "Success: no issues found in 1 source file"). It now asserts what mypy
// says: CLEAN. The isolation this test was written to protect is still
// pinned, by the scope-limitedness of the alias rather than by its absence --
// see AFunctionLocalClassIsNotVisibleFromASiblingFunction below, and
// AFunctionLocalClassDoesNotCorruptASameNamedTopLevelClass further down.
TEST(TypeChecker, AFunctionLocalClassesDoNotLeakOrOverwriteEachOther) {
    expect_clean(
        "def f() -> None:\n"
        "    class L:\n"
        "        def __init__(self) -> None:\n"
        "            self.a = 1\n"
        "def g() -> None:\n"
        "    class L:\n"
        "        def __init__(self) -> None:\n"
        "            self.b = 2\n"
        "    w = L()\n"
        "    y: int = w.b\n");
}

// The alias is
// SCOPE-LIMITED, so a local class is invisible from a SIBLING function that
// declares no class of its own under that name. Verified against real mypy
// 1.18.1, which agrees exactly: `d.py:5: error: Name "L" is not defined
// [name-defined]`. Without the removal half of LocalClassAliasGuard this
// would silently resolve as a constructor call instead.
TEST(TypeChecker, AFunctionLocalClassIsNotVisibleFromASiblingFunction) {
    const Checked checked = check_module(
        "def f() -> None:\n"
        "    class L:\n"
        "        pass\n"
        "def g() -> None:\n"
        "    v = L()\n");

    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "NameError");
    EXPECT_EQ(error.message, "name 'L' is not defined");
}

// Shadowing composes by stack discipline. An inner
// function's own same-named local class shadows the outer one's for exactly
// its own body -- so `w.b` resolves against INNER's L -- and the outer one
// is restored, not deleted, once inner's body walk is done, so `v.a` after
// it still resolves against OUTER's L. A LocalClassAliasGuard that merely
// ERASED each alias on teardown, instead of restoring the previous target,
// would make that trailing `L()` a false NameError. Verified mypy-clean
// against real mypy 1.18.1.
TEST(TypeChecker, ANestedFunctionsLocalClassShadowsTheEnclosingOnesOnlyForItsOwnBody) {
    expect_clean(
        "def outer() -> None:\n"
        "    class L:\n"
        "        def __init__(self) -> None:\n"
        "            self.a = 1\n"
        "    def inner() -> None:\n"
        "        class L:\n"
        "            def __init__(self) -> None:\n"
        "                self.b = 2\n"
        "        w = L()\n"
        "        y: int = w.b\n"
        "    v = L()\n"
        "    z: int = v.a\n");
}

// A function-local class shadows a MODULE-LEVEL
// class of the same name for the duration of that function, which is what
// makes the scoped alias outrank a live ClassTable entry under the identical
// spelling (see ClassTable::canonical_name's own precedence comment).
// Resolving to the module-level "L" instead would point `v.b` at the wrong
// class and produce a false attr-defined TypeError. Verified mypy-clean
// against real mypy 1.18.1.
TEST(TypeChecker, AFunctionLocalClassShadowsASameNamedModuleLevelClass) {
    expect_clean(
        "class L:\n"
        "    def __init__(self) -> None:\n"
        "        self.a = 1\n"
        "def f() -> None:\n"
        "    class L:\n"
        "        def __init__(self) -> None:\n"
        "            self.b = 2\n"
        "    v = L()\n"
        "    y: int = v.b\n");
}

// The two shadowing tests below each used
// only ONE of the two same-named classes inside the function -- the local one
// (above) or the module one (below) -- so neither covered the shape where
// BOTH are live at the same point: a value whose type was resolved OUTSIDE
// the function, used INSIDE it, while a local `class L` shadows the name.
//
// A `Class` Type carries a NAME STRING that is re-canonicalised at every
// use, so `obj`'s type -- bare `Class("L")`, canonicalised in Phase 2 before
// any alias existed -- re-resolved to the LOCAL class the moment it was read
// inside `f`, and `obj.b()` was a FALSE `"L" has no attribute "b"`. Both
// programs here are `Success: no issues found` under real mypy 1.18.1.
//
// Closed by ClassTable::shadowed_name: a MISS through a scoped alias retries
// against the entry that alias shadows instead of concluding the attribute
// does not exist. See that function's own comment for why the four
// membership queries take the fallback and constructor_type must not.
TEST(TypeChecker, AModuleLevelTypedValueKeepsItsOwnClassInsideAShadowingFunction) {
    expect_clean(
        "class L:\n"
        "    def b(self) -> int:\n"
        "        return 1\n"
        "obj = L()\n"
        "def f() -> None:\n"
        "    class L:\n"
        "        pass\n"
        "    y: int = obj.b()\n");
}

// The same defect reached through an ANNOTATION rather than an inferred
// module-level binding: `p: L` is resolved by AnnotationResolver in Phase 2,
// long before `f`'s body walk installs the scoped alias, so the parameter's
// type is likewise a bare `Class("L")` that re-canonicalises to the local
// class. Also mypy-clean.
TEST(TypeChecker, AParameterAnnotatedWithAModuleLevelClassSurvivesAShadowingLocalClass) {
    expect_clean(
        "class L:\n"
        "    def b(self) -> int:\n"
        "        return 1\n"
        "def f(p: L) -> None:\n"
        "    class L:\n"
        "        pass\n"
        "    y: int = p.b()\n");
}

// The regression guard for the ONE query deliberately excluded from the
// fallback. Had constructor_type taken it, the shadowed `L.__init__`'s
// parameter list would have become the local `L`'s, making this mypy-clean
// program a false "too few arguments" -- a NEW false positive, i.e. exactly
// what the scoped alias exists to remove. The bare `L()` inside `f` must keep
// constructing the LOCAL class, which takes no arguments.
TEST(TypeChecker, AShadowedClassesConstructorParametersDoNotReachTheLocalClass) {
    expect_clean(
        "class L:\n"
        "    def __init__(self, a: int) -> None:\n"
        "        self.a = a\n"
        "def f() -> None:\n"
        "    class L:\n"
        "        pass\n"
        "    v = L()\n"
        "    print(v)\n");
}

// A function-local
// class's bare name must NOT leak into ClassTable as a permanently live
// entry for the REST of the module's Phase-3 walk. Before that fix,
// `class L` inside `f` declared under the bare name "L" the first time
// Phase 3 reached it, so a module-level `L()` occurring TEXTUALLY AFTER `f`
// silently resolved as a constructor call instead of reporting NameError --
// an order-dependent regression from the original (correct) behaviour.
// The scoped alias keeps this correct for the same reason: the alias
// is removed when `f`'s own body walk ends, so nothing at module level can
// see it. Real mypy 1.18.1 agrees this one IS an error:
// `c6.py:4: error: Name "L" is not defined  [name-defined]`.
TEST(TypeChecker, AFunctionLocalClassBareNameDoesNotLeakToLaterModuleLevelCode) {
    const Checked checked = check_module(
        "def f() -> None:\n"
        "    class L:\n"
        "        pass\n"
        "v = L()\n");

    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "NameError");
    EXPECT_EQ(error.message, "name 'L' is not defined");
}

// Before this fix,
// a function-local class sharing a bare name with a real top-level class
// would have declared ITS OWN members onto the TOP-LEVEL class's ClassTable
// entry (both computed the same bare "C" qualified name), since ScopeStack
// never sees a class name and so cannot tell the two apart either. The
// isolated qualified name this fix gives the local class prevents that: the
// TOP-LEVEL C (constructed at module scope below) genuinely has no
// "local_only" attribute, so reading it is still reported.
TEST(TypeChecker, AFunctionLocalClassDoesNotCorruptASameNamedTopLevelClass) {
    const Checked checked = check_module(
        "class C:\n"
        "    def __init__(self) -> None:\n"
        "        self.real = 1\n"
        "def make() -> None:\n"
        "    class C:\n"
        "        def __init__(self) -> None:\n"
        "            self.local_only = 2\n"
        "c = C()\n"
        "z: int = c.local_only\n");

    EXPECT_EQ(only_error(checked).code, "TypeError");
}

// An EARLIER
// self.x = ... assignment (in a method occurring ABOVE this class-body
// annotation) declares "x" as int; declare_member has no collision
// detection of its own, so the conflicting `x: str` below it must not
// silently re-type the attribute with zero diagnostics.
//
// An earlier version of this check reported it at the
// ANNOTATION's line (4), treating the annotation as "expression" and the
// self-assignment's inferred type as "variable" -- verified against real
// mypy 1.18.1 (`mypy --strict` on this exact program) to be BACKWARDS on
// both counts:
//   case.py:3: error: Incompatible types in assignment (expression has type
//   "int", variable has type "str")  [assignment]
// mypy treats a class-body annotation as the DECLARED type of the attribute
// for the WHOLE class body regardless of where it appears textually, and
// reports the conflict at the ASSIGNMENT's own line (3, self.x = 5) with the
// assignment's inferred type as "expression" and the annotation's declared
// type as "variable" -- exactly the reverse of the original polarity and
// line. pre_collect_class_body now resolves and declares every direct
// class-body AnnAssign in its own sub-pass BEFORE any method's self.x scan,
// so this ordering-independent precedence holds regardless of which
// statement is textually first (see AClassBodyAnnotationAboveAConflicting
// SelfAssignmentIsReported below for the already-correct reverse ordering).
TEST(TypeChecker, AClassBodyAnnotationConflictingWithAnEarlierSelfAssignmentIsReported) {
    const Checked checked = check_module(
        "class C:\n"
        "    def m(self) -> None:\n"
        "        self.x = 5\n"
        "    x: str\n");

    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.line, 3);
    EXPECT_EQ(error.message,
              "incompatible types in assignment (expression has type \"int\", "
              "variable has type \"str\")");
}

//
// `mypy --strict` on this exact program (annotation ABOVE the conflicting
// self.x) reports:
//   case3.py:5: error: Incompatible types in assignment (expression has type
//   "int", variable has type "str")  [assignment]
// -- the SAME line/polarity convention as the "annotation below" case above,
// confirming the class-body annotation is authoritative and self.x is
// always the "expression" being checked against it, regardless of order.
TEST(TypeChecker, AClassBodyAnnotationAboveAConflictingSelfAssignmentIsReported) {
    const Checked checked = check_module(
        "class C:\n"
        "    x: str\n"
        "    def m(self) -> None:\n"
        "        self.x = 5\n");

    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.line, 4);
    EXPECT_EQ(error.message,
              "incompatible types in assignment (expression has type \"int\", "
              "variable has type \"str\")");
}

// A LOSING top-level class redefinition's own body
// is still checked (matching how a colliding top-level FunctionDef's body is
// still checked), but must no longer write onto the WINNING same-named
// class's ClassTable entry -- specifically, the loser's own __init__ must
// not override the winner's arity, and the loser's own extra attribute must
// not leak onto an instance of the winner. Before this fix, both classes
// resolved to the identical bare "C" qualified name.
TEST(TypeChecker, ALosingClassRedefinitionDoesNotOverrideTheWinningOnesConstructor) {
    const Checked checked = check_module(
        "class C:\n"
        "    def __init__(self, a: int) -> None:\n"
        "        self.a = a\n"
        "class C:\n"
        "    def __init__(self, a: int, b: int) -> None:\n"
        "        self.extra = b\n"
        "c = C(1)\n"
        "y: int = c.a\n");

    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "name \"C\" already defined on line 1");
}

// Verified: the class body is NOT in the method's lexical scope.
//
// PLAN DEFECT (reported, not silently patched): the brief's own version of
// this test used `return x`, which was VACUOUS AT THE TIME for the exact
// reason recorded on DirectRecursionIsClean and
// AClosureMayReadALocalAssignedAfterItsOwnDef above -- Return was not yet
// overridden (that landed in Task 20), so RecursiveVisitor's default just
// walked to the Name `x` via accept(), whose own visit() is RecursiveVisitor's
// no-op default; `x` never reached ExpressionTyper and the test could not
// have failed regardless of whether a method body actually skips the class
// scope. Rewritten to `print(x)`, the same established fix -- kept as-is now
// that Return IS real, since `m`'s `-> None` means restoring `return x` would
// ALSO report "no return value expected" alongside the NameError this test
// pins, breaking the single-diagnostic assertion below for a reason unrelated
// to what this test is actually about.
TEST(TypeChecker, AMethodBodyDoesNotSeeClassBodyNames) {
    const Checked checked = check_module(
        "class C:\n"
        "    x: int = 1\n"
        "    def m(self) -> None:\n"
        "        print(x)\n");

    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "NameError");
    // mypy emits TWO here -- name-defined plus a cascading no-any-return from
    // the poisoned Any. We emit ONE, because Unknown is absorbing. That is a
    // deliberate difference, not a gap in either compliance direction.
}

// Verified: a class body IS order-sensitive, and mypy uses name-defined here
// rather than used-before-def for the structurally identical mistake.
TEST(TypeChecker, AClassBodyIsOrderSensitive) {
    const Checked checked = check_module("class C:\n    a: int = b\n    b: int = 2\n");

    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "NameError");
    EXPECT_EQ(error.message, "name 'b' is not defined");
}

TEST(TypeChecker, ResolvesInheritedAttributes) {
    expect_clean("class B:\n    x: int\nclass D(B):\n    pass\nd = D()\ny: int = d.x\n");
}

// THE INVARIANT-(a) CLOSURE, end to end. Verified mypy-clean.
TEST(TypeChecker, AUserClassInheritingABuiltinIsAssignableToIt) {
    expect_clean("class Sub(int):\n    pass\nx: int = Sub()\n");
}

// Verified: e: Exception = ValueError() is mypy-clean -- the seeded bases at
// work.
TEST(TypeChecker, SeededBuiltinClassesAndTheirHierarchyResolve) {
    expect_clean("x: type\n");
    expect_clean("e: Exception = ValueError()\n");
    expect_clean("b: BaseException = ValueError()\n");
}

TEST(TypeChecker, DeclaresNestedClassesByQualifiedName) {
    expect_clean("class Outer:\n    class Inner:\n        pass\nx: Outer.Inner = Outer.Inner()\n");
}

// A nested class named but NOT called. Verified mypy-clean; measured with
// mypy 1.18.1, `reveal_type(Outer.Inner)` is `def () -> Outer.Inner`, the
// constructor signature, which is what this branch returns. This used to be
// `TypeError: "Outer" has no attribute "Inner"`: only the CALL shape
// `Outer.Inner()` was special-cased, so a bare reference fell into the
// member/method lookup, which has no notion of a nested class.
TEST(TypeChecker, ReferencingANestedClassWithoutCallingItIsClean) {
    expect_clean("class Outer:\n"
                 "    class Inner:\n"
                 "        def v(self) -> int:\n"
                 "            return 1\n"
                 "\n"
                 "\n"
                 "x = Outer.Inner\n"
                 "print(x)\n");
}

// A class OBJECT against the annotation `type`, in both spellings that
// produce one: a bare class name (type_of_name's carve-out) and a nested
// class (type_of_attribute's branch). Both are mypy-clean -- measured, mypy
// 1.18.1: `Success` for each -- and both are a Callable source against a
// Class("type") target, a pair is_subtype had NO arm for. So each drew a
// false "incompatible types in assignment (expression has type
// "Callable[[], Widget]", variable has type "type")". The nested-class one
// was broken BEFORE a bare class name typed as its constructor at all,
// since that branch already returned one; the bare one only became
// reachable once it did.
TEST(TypeChecker, AClassObjectSatisfiesATypeAnnotation) {
    expect_clean("class Widget:\n"
                 "    pass\n"
                 "\n"
                 "\n"
                 "x: type = Widget\n");
    expect_clean("class Outer:\n"
                 "    class Inner:\n"
                 "        pass\n"
                 "\n"
                 "\n"
                 "x: type = Outer.Inner\n");
}

// Calling through a class VALUE. mypy 1.18.1 reveals `w = Widget` as
// `def (n: int) -> Widget` and `w(1)` as `Widget`, so this must construct
// exactly as `Widget(1)` does -- arguments checked, instance returned, its
// attributes reachable. Before the constructor answer replaced
// Class("type"), `w(1)` reported `NotImplementedError: calling an instance
// of a user-defined class is not supported` (a missed error, so safe) and
// `w.n` reported a false `"type" has no attribute "n"` (a hard-invariant
// violation, so not).
TEST(TypeChecker, ACallThroughAClassValueConstructsTheInstance) {
    expect_clean("class Widget:\n"
                 "    def __init__(self, n: int) -> None:\n"
                 "        self.n = n\n"
                 "\n"
                 "\n"
                 "w = Widget\n"
                 "y: int = w(1).n\n");
}

// The arity check survives the indirection, which is what proves the
// constructor is carried through rather than replaced by a nullary or
// Unknown stand-in. mypy reports `Missing positional argument "n" in call to
// "Widget"` here; we name the CALLEE as the user spelled it (`w`), since a
// Callable value has no class name of its own to report.
TEST(TypeChecker, AnArityErrorThroughAClassValueIsStillReported) {
    const Checked checked = check_module("class Widget:\n"
                                         "    def __init__(self, n: int) -> None:\n"
                                         "        self.n = n\n"
                                         "\n"
                                         "\n"
                                         "w = Widget\n"
                                         "b = w()\n");

    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "too few arguments for \"w\"");
    EXPECT_EQ(error.line, 7);
}

// A FUNCTION-LOCAL class used as a value. Declared under a synthetic
// isolated ClassTable name and reached only through the scope-limited alias
// TypeChecker installs, so this pins that the carve-out passes the BARE
// identifier (which ClassTable::canonical_name re-routes) and not an
// already-resolved one. Verified mypy-clean; `reveal_type` is
// `def () -> Local@2`.
TEST(TypeChecker, AFunctionLocalClassNameUsedAsAValueIsClean) {
    expect_clean("def outer() -> None:\n"
                 "    class Local:\n"
                 "        pass\n"
                 "\n"
                 "    w = Local\n"
                 "    print(w())\n");
}

// Arbitrary nesting depth, both named and called: the class-object receiver
// check is recursive over the whole dotted chain, so `A.B.C` needs no
// per-depth special case. Verified mypy-clean.
TEST(TypeChecker, ADeeplyNestedClassResolvesAndConstructs) {
    expect_clean("class A:\n"
                 "    class B:\n"
                 "        class C:\n"
                 "            def v(self) -> int:\n"
                 "                return 1\n"
                 "\n"
                 "\n"
                 "y = A.B.C()\n"
                 "print(y.v())\n");
}

// A MEMBER reached through two class objects, which the same recursion makes
// reachable: `Outer.Inner.count` resolves against Outer.Inner's members
// rather than reporting that Outer has no attribute "Inner".
TEST(TypeChecker, AMemberOfANestedClassResolvesThroughTheClassObjects) {
    expect_clean("class Outer:\n"
                 "    class Inner:\n"
                 "        count: int = 0\n"
                 "\n"
                 "\n"
                 "print(Outer.Inner.count)\n");
}

// PRECEDENCE, which the recursion must not break: a local binding of the
// root name wins over the class-object reading, so the class-object path is
// not taken and `Outer` is the int parameter it was declared as. Reported as
// a modelling limit (attribute access on a builtin kind), never as a
// class-object resolution.
TEST(TypeChecker, ALocalBindingOfAClassNameStillWinsOverTheClassObjectPath) {
    const Checked checked = check_module("class Outer:\n"
                                         "    class Inner:\n"
                                         "        pass\n"
                                         "\n"
                                         "\n"
                                         "def f(Outer: int) -> None:\n"
                                         "    print(Outer.Inner)\n");
    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "NotImplementedError");
}

// The whole-module counterpart of ScopeStack's
// AClassBodyDoesNotSeeAnEnclosingClassBody: an inner class body reading the
// outer class body's name is a genuine NameError, which this checker used to
// miss entirely. Verified against mypy 1.18.1 ("Name \"x\" is not defined")
// and CPython 3.14, which raises NameError while creating the class.
TEST(TypeChecker, ANestedClassBodyCannotReadTheEnclosingClassBody) {
    const Checked checked = check_module("class C1:\n"
                                         "    x: int = 1\n"
                                         "\n"
                                         "    class C2:\n"
                                         "        y: int = x\n");
    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "NameError");
    EXPECT_EQ(error.message, "name 'x' is not defined");
    EXPECT_EQ(error.line, 5);
}

// Verified: c.x = 5 from OUTSIDE the class is attr-defined. The attribute set
// is closed at the class definition.
TEST(TypeChecker, ReportsAssigningANewAttributeFromOutside) {
    const Checked checked = check_module("class C:\n    pass\nc = C()\nc.x = 5\n");

    EXPECT_EQ(only_error(checked).code, "TypeError");
}

TEST(TypeChecker, ChecksMethodCallArgumentsWithSelfDropped) {
    expect_clean(
        "class C:\n"
        "    def m(self, a: int) -> int:\n"
        "        return a\n"
        "c = C()\n"
        "y: int = c.m(1)\n");

    const Checked checked = check_module(
        "class C:\n"
        "    def m(self, a: int) -> int:\n"
        "        return a\n"
        "c = C()\n"
        "c.m(1, 2)\n");
    EXPECT_EQ(only_error(checked).code, "TypeError");
}

// Task 19 gap 1 (recorded as pre-existing, closed by this task): a class-body
// AnnAssign used to bind into MODULE scope (there was no real Class scope to
// bind into instead), so a module-level name reused after the class reported
// a false "already defined" redefinition against it. A real Class scope
// isolates the two.
TEST(TypeChecker, AClassBodyAnnotationDoesNotLeakIntoModuleScope) {
    expect_clean("class A:\n    x: int\nx: str = \"s\"\n");
}

// The ORIGINAL
// version of this test (named ALosingClassRedefinitionDoesNotClobberThe
// WinningOnesMembers) was VACUOUS -- it passed identically with or without
// Task 19's own fix. Phase 3 runs entirely AFTER Phase 1 declares every
// class, so by the time the LOSING class's declare() call would have run,
// the WINNING class's ClassTable entry had ZERO members yet (nothing is
// declared until Phase 3 walks a body); there was never anything for the
// loser to clobber. What the fix ACTUALLY prevents is the loser wiping the
// winner's BASE LIST: declare() unconditionally overwrites `bases` with no
// collision detection of its own, so a losing SECOND `class C:` (no bases)
// sharing the SAME "C" entry as the winning `class C(B):` would silently
// sever C's inheritance from B, making `c.x` (a member B alone declares) a
// false attr-defined TypeError. Verified this fails without the fix
// (temporarily reverting collect_classes' collided_top_level_ skip
// reproduces exactly this false TypeError; restored after confirming it).
TEST(TypeChecker, ALosingClassRedefinitionDoesNotClobberTheWinningOnesBases) {
    const Checked checked = check_module(
        "class B:\n"
        "    x: int\n"
        "class C(B):\n"
        "    pass\n"
        "class C:\n"
        "    pass\n"
        "c = C()\n"
        "z: int = c.x\n");

    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "name \"C\" already defined on line 3");
}

//
// bind_annotation handles a function-local AnnAssign target (visit(AnnAssign)
// routes any non-module-level Name target here) and used to compare
// `existing.binding->declared_line == line` directly, with no order_exempt
// involvement -- so a same-line, order_exempt PARAMETER re-annotated with
// `:` was mistaken for bind_annotation's own "still-unfilled placeholder"
// case and silently REBOUND, discarding the parameter's real annotation with
// zero diagnostics. Verified against mypy 1.18.1: re-annotating an existing
// binding (parameter or otherwise) is `error: Name "x" already defined on
// line 1`, the SAME redefinition wording every other same-line collision in
// this file already uses -- not an assignment-error, since mypy reports the
// redefinition alone. This pins the one-line form; the next test pins the
// two-line form reports the identical thing.
TEST(TypeChecker, AOneLineDefReannotatingItsOwnParameterIsARedefinition) {
    const Checked checked = check_module("def f(x: int) -> None: x: str = \"s\"\n");

    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "name \"x\" already defined on line 1");
}

// The two-line form takes bind_annotation's OTHER branch from the start
// (existing.binding->declared_line == 1, the def's own line, is already !=
// 2, the AnnAssign's own line, with no order_exempt involvement needed) --
// this pins that it reports the SAME redefinition wording as the one-line
// form above, so the two forms do not silently diverge.
TEST(TypeChecker, AMultiLineDefReannotatingItsOwnParameterIsARedefinition) {
    const Checked checked = check_module("def f(x: int) -> None:\n    x: str = \"s\"\n");

    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "name \"x\" already defined on line 1");
}

// ---------------------------------------------------------------------------
// Task 20: control flow, Return, and return-path checking.
// ---------------------------------------------------------------------------

TEST(TypeChecker, ChecksControlFlowBodies) {
    expect_clean("c: bool = True\nif c:\n    x: int = 1\nelse:\n    y: int = 2\n");
    expect_clean("c: bool = True\nwhile c:\n    x: int = 1\n");
    expect_clean("for i in range(3):\n    x: int = i\n");
    expect_clean("for i in range(3):\n    break\nelse:\n    pass\n");
}

// Truthiness is universal -- verified `if w:` clean on a plain user class.
TEST(TypeChecker, AnyConditionTypeIsAcceptable) {
    expect_clean("xs: list[int] = []\nif xs:\n    pass\n");
    expect_clean("class C:\n    pass\nc = C()\nif c:\n    pass\n");
}

TEST(TypeChecker, BindsTheForTargetToTheElementType) {
    expect_clean("for s in \"abc\":\n    t: str = s\n");
    expect_clean("d: dict[str, int] = {}\nfor k in d:\n    s: str = k\n");
}

// A for target does NOT get its own scope -- verified: reading it after the
// loop is clean under --strict.
TEST(TypeChecker, TheForTargetSurvivesTheLoop) {
    expect_clean("for i in range(3):\n    pass\nx: int = i\n");
}

// A one-line `for` suite reading its own
// target was a false NameError before this fix -- the body's ExprStmt sets
// statement_line_ to the SAME line the for-loop bound `i` at (there is no
// separate body line to be strictly greater, exactly the one-line-def shape
// Task 18 already fixed for parameters), so the ordinary `declared_line >=
// statement_line_` ordering check misfired as "name 'i' is used before
// definition" on mypy-clean code. Verified against mypy 1.18.1: --strict
// clean. Also verified directly against the compiled binary (see the fix
// earlier) with the same fixture.
TEST(TypeChecker, AOneLineForSuiteReadingItsOwnTargetIsClean) {
    expect_clean("for i in range(3): print(i)\n");
}

// The multi-line form was already clean before this fix (the body's own line
// is genuinely greater than the for-loop's line, so the bug never reached
// it) -- pinned here anyway so both forms have direct, explicit coverage
// rather than relying on BindsTheForTargetToTheElementType's incidental
// reads. Verified mypy-clean.
TEST(TypeChecker, AMultiLineForSuiteReadingItsOwnTargetIsClean) {
    expect_clean("for i in range(3):\n    print(i)\n");
}

// The other half of the same rule: a read BEFORE the loop is untouched by the
// order_exempt fix, because `i` is not bound AT ALL yet at that point --
// neither pre-bind pass covers a For target, so there is no placeholder for
// an early read to find, unlike an ordinary module-level assignment (compare
// ReportsAModuleLevelUseBeforeDefinition's "used before definition", which
// DOES have a pre-bind placeholder). "is not defined" is therefore the
// correct outcome, not "used before definition".
TEST(TypeChecker, AReadBeforeTheForLoopStillReportsNotDefined) {
    const Checked checked = check_module("print(i)\nfor i in range(3):\n    pass\n");

    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "NameError");
    EXPECT_EQ(error.message, "name 'i' is not defined");
}

TEST(TypeChecker, ReportsIteratingANonIterable) {
    const Checked checked = check_module("for i in 1:\n    pass\n");
    EXPECT_EQ(only_error(checked).code, "TypeError");
}

// A user class instance is NEVER a "not iterable" TypeError, in a `for` or in
// a comprehension. Verified against mypy 1.18.1: the Bag/Counter pair below
// is CLEAN with no import at all -- mypy matches __iter__/__next__
// structurally -- so a TypeError here is a false positive on ordinary
// mypy-clean Python. Reported as the modelling limit it is instead.
TEST(TypeChecker, IteratingAUserClassInstanceIsUnsupportedNotAnError) {
    const std::string source = "class Counter:\n"
                               "    def __init__(self) -> None:\n"
                               "        self.n = 0\n"
                               "\n"
                               "    def __next__(self) -> int:\n"
                               "        self.n = self.n + 1\n"
                               "        return self.n\n"
                               "\n"
                               "\n"
                               "class Bag:\n"
                               "    def __iter__(self) -> Counter:\n"
                               "        return Counter()\n"
                               "\n"
                               "\n"
                               "for v in Bag():\n"
                               "    print(v)\n";
    const Checked checked = check_module(source);
    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "NotImplementedError");
    EXPECT_EQ(error.message, "iterating an instance of a user-defined class is not supported");
}

TEST(TypeChecker, AComprehensionOverAUserClassInstanceIsUnsupportedNotAnError) {
    const Checked checked = check_module("class Bag:\n"
                                         "    pass\n"
                                         "\n"
                                         "\n"
                                         "b = Bag()\n"
                                         "xs = [v for v in b]\n");
    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "NotImplementedError");
    EXPECT_EQ(error.message, "iterating an instance of a user-defined class is not supported");
}

// And where the inherited builtin pins the element type, the precise answer
// comes through -- clean, with the element usable at that type. Verified
// mypy-clean, revealing str for the `class Names(str)` case.
TEST(TypeChecker, IteratingAClassThatInheritsABuiltinIsCleanAtTheInheritedElementType) {
    expect_clean("class Names(str):\n"
                 "    pass\n"
                 "\n"
                 "\n"
                 "def f(names: Names) -> None:\n"
                 "    for ch in names:\n"
                 "        s: str = ch\n"
                 "        print(s)\n");
}

// mypy ACCEPTS tuple targets, so this must be NotImplementedError.
TEST(TypeChecker, ReportsATupleForTargetAsUnsupported) {
    const Checked checked = check_module("for a, b in [(1, 2)]:\n    pass\n");

    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "NotImplementedError");
    EXPECT_EQ(error.message, "tuple targets in for loops are not supported");
}

// Without binding the tuple target's elements to
// Unknown, a REAL use inside the body (unlike every other tuple-for-target
// fixture in this file, which is pass-only) would cascade its own NameError
// on top of the NotImplementedError above -- Unknown is absorbing, so
// `x: int = a` triggers no compatibility check of its own. Mirrors
// assign_tuple's own arity-mismatch fallback, which sets this exact
// precedent for the same reason.
TEST(TypeChecker, ATupleForTargetsElementsAreBoundToUnknownRatherThanCascading) {
    const Checked checked = check_module("for a, b in [(1, 2)]:\n    x: int = a\n");

    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "NotImplementedError");
}

// Verified: reported at the DEF line. mypy's code is empty-body for a
// pass-only body and `return` for a fall-through; we use TypeError for both.
TEST(TypeChecker, ReportsAMissingReturnStatement) {
    const Checked checked = check_module("def f() -> int:\n    pass\n");

    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "missing return statement");
    EXPECT_EQ(error.line, 1) << "reported at the def line, matching mypy";
}

TEST(TypeChecker, BothBranchesReturningIsExhaustive) {
    expect_clean("def f(c: bool) -> int:\n    if c:\n        return 1\n    else:\n        return 2\n");
}

TEST(TypeChecker, AnIfWithoutAnElseIsNotExhaustive) {
    const Checked checked =
        check_module("def f(c: bool) -> int:\n    if c:\n        return 1\n");
    EXPECT_EQ(only_error(checked).code, "TypeError");
}

// Verified CLEAN: `while True` with no reachable break is treated as
// non-terminating, so fall-through is unreachable -- even when the loop might
// spin forever without returning.
TEST(TypeChecker, WhileTrueSatisfiesTheReturnCheck) {
    expect_clean("def f() -> int:\n    while True:\n        return 1\n");
    expect_clean("def f(c: bool) -> int:\n    while True:\n        if c:\n            return 1\n");
}

// The syntactic approximation. A break makes the loop escapable, so
// fall-through is reachable and the function needs a return after it.
TEST(TypeChecker, WhileTrueWithABreakDoesNotSatisfyTheReturnCheck) {
    const Checked checked = check_module(
        "def f(c: bool) -> int:\n"
        "    while True:\n"
        "        if c:\n"
        "            break\n"
        "        return 1\n");
    EXPECT_EQ(only_error(checked).code, "TypeError");
}

// Verified: a conditional while and a for are both assumed skippable.
TEST(TypeChecker, ASkippableLoopDoesNotSatisfyTheReturnCheck) {
    const Checked conditional =
        check_module("def f(c: bool) -> int:\n    while c:\n        return 1\n");
    EXPECT_EQ(only_error(conditional).code, "TypeError");

    const Checked loop = check_module(
        "def f(xs: list[int]) -> int:\n    for x in xs:\n        return 1\n");
    EXPECT_EQ(only_error(loop).code, "TypeError");
}

TEST(TypeChecker, ANoneReturningFunctionNeedsNoReturn) {
    expect_clean("def f() -> None:\n    pass\n");
}

TEST(TypeChecker, ReportsABareReturnInANonNoneFunction) {
    const Checked checked = check_module("def f() -> int:\n    return\n");

    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "return value expected");
}

TEST(TypeChecker, ReportsAValueReturnedFromANoneFunction) {
    const Checked checked = check_module("def f() -> None:\n    return 5\n");

    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "no return value expected");
}

// The counterpart the check above used to get wrong: a value is PRESENT, but
// its type is None, and mypy 1.18.1 accepts both of these cleanly. The old
// rule reported whenever a value existed at all, without ever looking at its
// type, so `return None` -- an ordinary early exit -- was a false TypeError.
TEST(TypeChecker, ReturningANoneValuedExpressionFromANoneFunctionIsClean) {
    expect_clean("def maybe(x: int) -> None:\n"
                 "    if x < 0:\n"
                 "        return None\n"
                 "    print(x)\n");
    expect_clean("def g() -> None:\n"
                 "    print(1)\n"
                 "\n"
                 "\n"
                 "def forward() -> None:\n"
                 "    return g()\n");
}

// Pins the full message text -- the one message this
// task invented -- rather than only its code, matching every other new
// message in this file.
TEST(TypeChecker, ReportsAnIncompatibleReturnValue) {
    const Checked checked = check_module("def f() -> int:\n    return \"s\"\n");

    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "incompatible return value type (got \"str\", expected \"int\")");
}

// The ORIGINAL fixture here (`def f() -> list[int]:
// return []`) was VACUOUS -- it passes with or without the context
// propagation this test is supposed to pin, since an empty display with no
// context takes the no-context path and returns Unknown, which
// visit(Return)'s `value_type.kind != Unknown` guard then skips entirely.
// This version uses a MISTYPED element so the check can only pass by
// actually threading list[int] into the list display as context: without
// that context, `["s"]` types as list[str] with no declared element type to
// check `"s"` against, producing the DIFFERENT "incompatible return value
// type" message instead of this per-item one -- verified by temporarily
// removing the context propagation and confirming the message changes.
TEST(TypeChecker, TheDeclaredReturnTypeIsTheValuesContext) {
    const Checked checked = check_module("def f() -> list[int]:\n    return [\"s\"]\n");

    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "list item 0 has incompatible type \"str\"; expected \"int\"");
}

// Verified: x: float = f() where f() -> int is clean -- the numeric tower
// applies to returns too.
TEST(TypeChecker, ReturnValuesFollowTheNumericTower) {
    expect_clean("def f() -> float:\n    return 1\n");
}

// Not exercised by the brief's own test list above: a break belonging to a
// NESTED loop must not count as a reachable break for the OUTER `while
// True`, per contains_reachable_break's own contract. Verified against mypy
// 1.18.1: this exact fixture is --strict clean.
TEST(TypeChecker, ABreakInANestedLoopDoesNotEscapeTheOuterWhileTrue) {
    expect_clean(
        "def f(xs: list[int]) -> int:\n"
        "    while True:\n"
        "        for x in xs:\n"
        "            break\n"
        "        return 1\n");
}

// The OPPOSITE case from the test above -- a break in
// a nested loop's own ORELSE (not its body) targets the ENCLOSING loop, since
// a loop's `else` clause runs OUTSIDE that loop's own break scope (which is
// exactly why a top-level `for x in []: pass` / `else: break` is a plain
// `SyntaxError: 'break' outside loop`, not a compile error about the for
// loop). contains_reachable_break must recurse into a nested loop's orelse
// even though it does not recurse into its body. Verified against mypy
// 1.18.1: `error: Missing return statement  [return]`.
TEST(TypeChecker, ABreakInANestedLoopsOrelseEscapesTheOuterLoop) {
    const Checked checked = check_module(
        "def f(xs: list[int]) -> int:\n"
        "    while True:\n"
        "        for x in xs:\n"
        "            pass\n"
        "        else:\n"
        "            break\n");

    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "missing return statement");
}

// current_return_type_ is a single member restored by ReturnContextGuard, not
// a stack a naive reader might assume is unnecessary for a single level of
// nesting -- this pins that a nested def's return type does not leak back
// out to the ENCLOSING function once the nested def's own body walk is done:
// `outer`'s trailing bare `return` must be checked against None (outer's own
// declared type), not int (inner's).
TEST(TypeChecker, ANestedDefsReturnTypeDoesNotLeakToTheEnclosingFunction) {
    expect_clean(
        "def outer() -> None:\n"
        "    def inner() -> int:\n"
        "        return 1\n"
        "    inner()\n"
        "    return\n");
}

// ---------------------------------------------------------------------------
// A leak of TypeChecker's internal synthetic-isolation name into a
// user-facing diagnostic. Every other defect of this family has its test
// placed next to the one it amends, above; this one has no predecessor to
// sit next to, so it lives here.
// ---------------------------------------------------------------------------

// `type.name`/`ClassTable`'s own qualified name for
// an ISOLATED class (here, a LOSING top-level redefinition, which is one of
// the two isolation mechanisms) embeds TypeChecker's internal
// "<tag>#<line>#" isolation
// prefix. Before this fix, that prefix leaked VERBATIM into any diagnostic
// naming the class -- reachable via attr-defined, assignment, arg-type,
// return-type and operator messages, i.e. every path through type_name PLUS
// the two raw uses of a Class's `.name` in expression_typer.cpp/
// expression_typer_calls.cpp -- not only when `self` is the subject. This
// asserts the MESSAGE, not merely the diagnostic code (a code-only assertion
// would pass identically whether or not the leak were fixed): the loser's
// own body is still checked, so a genuine error INSIDE it (here,
// attr-defined on a name the loser itself never declares) must name the
// class as "C" -- exactly as the user wrote it -- never the internal
// "<shadowed-class>#4#C" ClassTable key.
TEST(TypeChecker, ASyntheticIsolationPrefixDoesNotLeakIntoAnAttrDefinedMessage) {
    const Checked checked = check_module(
        "class C:\n"
        "    def __init__(self) -> None:\n"
        "        self.a = 1\n"
        "class C:\n"
        "    def m(self) -> None:\n"
        "        y: int = self.missing\n");

    EXPECT_EQ(checked.diagnostics.size(), 2u);
    ASSERT_EQ(checked.diagnostics.size(), 2u);
    // Diagnostic 1: scan_top_level_names' own redefinition report (line 4,
    // module-level, unrelated to this finding -- see
    // ALosingClassRedefinitionDoesNotOverrideTheWinningOnesConstructor for
    // that message's own dedicated coverage).
    EXPECT_EQ(checked.diagnostics[0].code, "TypeError");
    // Diagnostic 2: the loser's own attr-defined miss, inside its isolated
    // body -- this is the one the leak reaches.
    EXPECT_EQ(checked.diagnostics[1].code, "TypeError");
    EXPECT_EQ(checked.diagnostics[1].message, "\"C\" has no attribute \"missing\"");
}

// Python introduces no scope for an `if` block, so a class defined inside one
// binds its name in MODULE scope. Verified against mypy 1.18.1: clean, with no
// "possibly undefined" complaint, and `reveal_type(Bag)` is `def () -> Bag`.
TEST(TypeChecker, AClassDefinedInsideAnIfIsDeclaredInModuleScope) {
    expect_clean("FLAG = True\n"
                 "if FLAG:\n"
                 "    class Bag:\n"
                 "        def __init__(self) -> None:\n"
                 "            self.n = 0\n"
                 "def f() -> None:\n"
                 "    b = Bag()\n"
                 "    print(b.n)\n");
}

// A class nested in an `if` inside a CLASS body is declared under its
// qualified name too -- same rule, one level down.
TEST(TypeChecker, AClassInsideAnIfInAClassBodyIsDeclaredQualified) {
    expect_clean("FLAG = True\n"
                 "class Outer:\n"
                 "    if FLAG:\n"
                 "        class Inner:\n"
                 "            pass\n"
                 "x: Outer.Inner = Outer.Inner()\n"
                 "print(x)\n");
}

// Verified against mypy 1.18.1: `Name "Bag" already defined on line 3
// [no-redef]` on the `else` branch, and the FIRST definition wins wholesale
// -- no union.
TEST(TypeChecker, TwoSameNamedClassesInIfElseAreARedefinition) {
    const Checked checked = check_module("FLAG = True\n"
                                         "if FLAG:\n"
                                         "    class Bag:\n"
                                         "        pass\n"
                                         "else:\n"
                                         "    class Bag:\n"
                                         "        pass\n");
    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "name \"Bag\" already defined on line 3");
    EXPECT_EQ(error.line, 6);
}

// A CLASS gets no conditional-definition allowance even against a flat `def`.
// Verified: mypy reports `Name "Bag" already defined on line 3 [no-redef]`.
TEST(TypeChecker, AClassInsideAnIfCollidesWithAFlatDef) {
    const Checked checked = check_module("FLAG = True\n"
                                         "def Bag() -> int:\n"
                                         "    return 1\n"
                                         "if FLAG:\n"
                                         "    class Bag:\n"
                                         "        pass\n");
    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "name \"Bag\" already defined on line 2");
}

// THE COUNTERPART, and the reason the collision rule is not simply "any two
// definitions of one name". Verified against mypy 1.18.1: BOTH of these are
// `Success` -- mypy allows a conditional FUNCTION redefinition. Reporting
// either would be a false TypeError on mypy-clean code.
TEST(TypeChecker, AConditionalDefDoesNotCollideWithAFlatDef) {
    expect_clean("FLAG = True\n"
                 "def f() -> int:\n"
                 "    return 1\n"
                 "if FLAG:\n"
                 "    def f() -> int:\n"
                 "        return 2\n");
}

TEST(TypeChecker, TwoConditionalDefsOfOneNameDoNotCollide) {
    expect_clean("FLAG = True\n"
                 "if FLAG:\n"
                 "    def f() -> int:\n"
                 "        return 1\n"
                 "    def f() -> int:\n"
                 "        return 2\n"
                 "print(f())\n");
}

// Unchanged by the new recursion: two FLAT defs still collide. Verified:
// `Name "f" already defined on line 1 [no-redef]`.
TEST(TypeChecker, TwoFlatDefsOfOneNameStillCollide) {
    const Checked checked = check_module("def f() -> int:\n"
                                         "    return 1\n"
                                         "def f() -> int:\n"
                                         "    return 2\n");
    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.message, "name \"f\" already defined on line 1");
}

// Python introduces no scope for an `if` block, so a `def` inside one binds
// its name in MODULE scope -- the exact counterpart of a `class` there.
// Verified against mypy 1.18.1 (`Success`) and CPython (prints 1). The
// signature pre-pass walked the module body flat, and a def's own name is
// bound by its visit only when the current scope is Function, so a
// conditional def was never bound at all and every call was a false
// NameError.
TEST(TypeChecker, ADefDefinedInsideAnIfIsBoundInModuleScope) {
    expect_clean("FLAG = True\n"
                 "if FLAG:\n"
                 "    def f() -> int:\n"
                 "        return 1\n"
                 "print(f())\n");
}

// Its SIGNATURE is bound, not merely its name: a wrong-typed argument at the
// call site is still caught.
TEST(TypeChecker, AConditionalDefsSignatureIsCheckedAtItsCallSite) {
    const Checked checked = check_module("FLAG = True\n"
                                         "if FLAG:\n"
                                         "    def f(n: int) -> int:\n"
                                         "        return n\n"
                                         "print(f(\"s\"))\n");
    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message,
              "argument 1 to \"f\" has incompatible type \"str\"; expected \"int\"");
}

// A closure reading a conditional def defined BELOW it resolves outward and is
// never order-checked, so this is clean. Verified: mypy `Success`.
TEST(TypeChecker, AFunctionCanCallAConditionalDefDefinedBelowIt) {
    expect_clean("FLAG = True\n"
                 "def g() -> int:\n"
                 "    return f()\n"
                 "if FLAG:\n"
                 "    def f() -> int:\n"
                 "        return 1\n"
                 "print(g())\n");
}

// THE HAZARD. Two conditional defs of one name is mypy-CLEAN (measured:
// `Success`), so the second binding must NOT report a redefinition. The first
// definition wins and the second is skipped silently -- if their signatures
// genuinely disagree that is a missed error, which is the safe direction and
// the same one the top-level name scan already takes for its own
// conditional-redefinition allowance.
TEST(TypeChecker, TwoConditionalDefsOfOneNameBindWithoutReporting) {
    expect_clean("FLAG = True\n"
                 "if FLAG:\n"
                 "    def f() -> int:\n"
                 "        return 1\n"
                 "else:\n"
                 "    def f() -> int:\n"
                 "        return 2\n"
                 "print(f())\n");
}

// A read ABOVE a conditional def now says "used before definition" rather than
// "not defined" -- both are errors and mypy reports one too
// (`Name "f" is used before definition`), so this is a wording improvement
// that falls out of the def finally having a Binding with its own line.
TEST(TypeChecker, AReadAboveAConditionalDefIsUsedBeforeDefinition) {
    const Checked checked = check_module("FLAG = True\n"
                                         "print(f())\n"
                                         "if FLAG:\n"
                                         "    def f() -> int:\n"
                                         "        return 1\n");
    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "NameError");
    EXPECT_EQ(error.message, "name 'f' is used before definition");
}

// A conditional annotated assignment is bound by the signature pre-pass
// exactly like a flat one, so a read above it is a same-scope, order-checked
// read against a real Binding -- "used before definition", not "not defined".
// Verified against mypy 1.18.1: `Name "y" is used before definition
// [used-before-def]`. Both tools error; this is a wording match, and both
// reject the program mypy does, so nothing mypy accepts starts failing.
TEST(TypeChecker, ReadAboveAConditionalAnnAssignIsUsedBeforeDefinition) {
    const Checked checked = check_module("FLAG = True\n"
                                         "print(y)\n"
                                         "if FLAG:\n"
                                         "    y: int = 5\n");
    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "NameError");
    EXPECT_EQ(error.message, "name 'y' is used before definition");
}

// A conditional PLAIN assignment is a different story from the AnnAssign
// above, because the assignment pre-pass walks `module.body()` flat and so
// installs no placeholder for one written inside an `if` -- the read falls
// through to "not defined" instead of being order-checked. Measured on this
// exact program:
//   mypy --strict: Name "x" is used before definition  [used-before-def]
//   CPython:       NameError: name 'x' is not defined
//   cythonpp:      2:7: error: NameError: name 'x' is not defined
// All three reject the program, so this is a WORDING gap, not a compliance
// one: nothing mypy accepts is refused, and nothing mypy refuses is accepted.
// Recursing the pre-pass to close the wording gap was tried and reverted --
// the wider traversal made an ordinary loop read a false NameError on a
// program both oracles accept; see pre_bind_assignment_targets.
TEST(TypeChecker, ReadAboveAConditionalAssignIsNotDefined) {
    const Checked checked = check_module("FLAG = True\n"
                                         "print(x)\n"
                                         "if FLAG:\n"
                                         "    x = 5\n");
    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "NameError");
    EXPECT_EQ(error.message, "name 'x' is not defined");
    EXPECT_EQ(error.line, 2);
}

// A conditional plain assignment binds cleanly when the ordinary walk reaches
// it, and a read BELOW it resolves. Also covers an if/else pair assigning one
// name (first branch wins, second is an ordinary compatible reassignment) and
// a conditional assignment of a name also assigned flat. All three measured
// mypy-clean and CPython-clean.
TEST(TypeChecker, AConditionalAssignmentBindsAndReadsCleanly) {
    expect_clean("FLAG = True\n"
                 "if FLAG:\n"
                 "    x = 5\n"
                 "print(x + 1)\n");
    expect_clean("FLAG = True\n"
                 "if FLAG:\n"
                 "    x = 5\n"
                 "else:\n"
                 "    x = 6\n"
                 "print(x + 1)\n");
    expect_clean("FLAG = True\n"
                 "if FLAG:\n"
                 "    x = 5\n"
                 "x = 6\n"
                 "print(x + 1)\n");
}

// ... and the branches disagreeing on type is still a reported incompatible
// assignment, at the SECOND branch, matching mypy exactly (measured:
// `p.py:5: error: Incompatible types in assignment (expression has type
// "str", variable has type "int")  [assignment]`). The first branch's
// inferred type is sticky, which is what makes the second a check rather than
// a silent rebind.
TEST(TypeChecker, IfElseBranchesAssigningIncompatibleTypesIsATypeError) {
    const Checked checked = check_module("FLAG = True\n"
                                         "if FLAG:\n"
                                         "    x = 5\n"
                                         "else:\n"
                                         "    x = \"s\"\n"
                                         "print(x)\n");
    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message,
              "incompatible types in assignment (expression has type \"str\", variable has type "
              "\"int\")");
    EXPECT_EQ(error.line, 5);
}

// A conditional annotated assignment now collides with a LATER flat def of the
// same name, reported at the def and pointing back to the annotation's own
// line -- because the annotation binds during this same pre-pass, before the
// def's own bind is attempted, exactly like two flat statements would.
// Verified against mypy 1.18.1: `Name "y" already defined on line 3
// [no-redef]`, reported on the def's own line (4), matching both the code and
// the line this test expects.
TEST(TypeChecker, ConditionalAnnAssignCollidesWithLaterFlatDef) {
    const Checked checked = check_module("FLAG = True\n"
                                         "if FLAG:\n"
                                         "    y: int = 5\n"
                                         "def y() -> int:\n"
                                         "    return 1\n");
    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "name \"y\" already defined on line 3");
    EXPECT_EQ(error.line, 4);
}

// A conditional def colliding with an EARLIER conditional annotated
// assignment of the same name must still report -- mypy's allowance for a
// conditional function redefinition applies only when the earlier binding was
// ALSO a def, and an annotated assignment is not one. Verified against mypy
// 1.18.1: `Incompatible redefinition (redefinition with type "Callable[[],
// int]", original type "int")  [misc]` at the def's own line (5). cythonpp
// keeps its own settled wording rather than mypy's, but both tools error.
TEST(TypeChecker, ConditionalAnnAssignCollidesWithLaterConditionalDef) {
    const Checked checked = check_module("FLAG = True\n"
                                         "if FLAG:\n"
                                         "    y: int = 5\n"
                                         "if FLAG:\n"
                                         "    def y() -> int:\n"
                                         "        return 1\n");
    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "name \"y\" already defined on line 3");
    EXPECT_EQ(error.line, 5);
}

// The same collision with a FLAT annotated assignment on the earlier side --
// pinned separately from the conditional-earlier-side case above because a
// flat AnnAssign followed by a conditional def is the shape a coarser
// "either side conditional" suppression rule would wrongly swallow (the def
// is the only conditional statement here). Verified against mypy 1.18.1:
// the same `Incompatible redefinition` error, at the def's own line (4).
TEST(TypeChecker, FlatAnnAssignCollidesWithLaterConditionalDef) {
    const Checked checked = check_module("FLAG = True\n"
                                         "y: int = 5\n"
                                         "if FLAG:\n"
                                         "    def y() -> int:\n"
                                         "        return 1\n");
    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "name \"y\" already defined on line 2");
    EXPECT_EQ(error.line, 4);
}

// The reverse order: a conditional def followed by a colliding conditional
// annotated assignment. Proves the rule is about the EARLIER binding's kind,
// not just "one side is conditional" -- unlike the two-conditional-defs case,
// which stays silent, this one must still report because the earlier
// binding is a variable, not a def. Verified against mypy 1.18.1:
// `Name "y" already defined on line 3  [no-redef]`, at the AnnAssign's own
// line (6).
TEST(TypeChecker, ConditionalDefCollidesWithLaterConditionalAnnAssign) {
    const Checked checked = check_module("FLAG = True\n"
                                         "if FLAG:\n"
                                         "    def y() -> int:\n"
                                         "        return 1\n"
                                         "if FLAG:\n"
                                         "    y: int = 5\n");
    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "name \"y\" already defined on line 3");
    EXPECT_EQ(error.line, 6);
}

// UNCHANGED GUARDS -- each is a row of the measured matrix that already worked,
// and this task must not disturb any of them.

// A flat AnnAssign followed by a flat def of the same name still collides --
// this is the ONE collision that reaches the signature pass's own bind check,
// and gating that report on flatness must not remove it. Confirm the exact
// wording against the current binary before trusting this expectation.
TEST(TypeChecker, AnAnnotatedNameFollowedByAFlatDefStillCollides) {
    const Checked checked = check_module("f: int = 1\n"
                                         "def f() -> int:\n"
                                         "    return 2\n");
    EXPECT_FALSE(checked.diagnostics.empty());
    EXPECT_EQ(checked.diagnostics.front().code, "TypeError");
}

// An if/else pair of ANNOTATED assignments still reports, matching mypy's
// `Name "y" already defined on line 3 [no-redef]` (verified against mypy
// 1.18.1). The signature pass's AnnAssign branch binds a conditional
// annotation exactly like a flat one, so the second one still collides with
// the first.
TEST(TypeChecker, TwoConditionalAnnotatedAssignmentsStillCollide) {
    const Checked checked = check_module("FLAG = True\n"
                                         "if FLAG:\n"
                                         "    y: int = 5\n"
                                         "else:\n"
                                         "    y: int = 6\n"
                                         "print(y)\n");
    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "name \"y\" already defined on line 3");
}

// A conditional def inside a `def` already worked and must keep working: the
// enclosing function's own scope is current, so the ordinary binding path
// covers it.
TEST(TypeChecker, AConditionalDefInsideAFunctionStillWorks) {
    expect_clean("FLAG = True\n"
                 "def outer() -> int:\n"
                 "    if FLAG:\n"
                 "        def inner() -> int:\n"
                 "            return 1\n"
                 "        return inner()\n"
                 "    return 0\n"
                 "print(outer())\n");
}

// A method body does not execute at class-definition time, so a class
// referenced from ABOVE its own definition is fully usable -- verified
// against mypy 1.18.1 (`Success`, with reveal_type(i) revealing
// "<module>.Item" and reveal_type(i.n) revealing "builtins.int"), and
// correct at runtime too. Member
// collection used to happen only when the ordinary walk reached the
// declaring ClassDef, so a reference from above saw a class with no members.
TEST(TypeChecker, AClassReferencedAboveItsDefinitionHasItsMembers) {
    expect_clean("class Cache:\n"
                 "    def use(self) -> int:\n"
                 "        i = Item()\n"
                 "        return i.n\n"
                 "class Item:\n"
                 "    def __init__(self) -> None:\n"
                 "        self.n = 0\n");
}

// A class-body annotation is the DECLARED type of that attribute for the
// whole class body regardless of textual position, so a subclass's narrowing
// annotation must be installed before ANY of its methods are walked.
// Verified: mypy 1.18.1 says `Success` for the reader ABOVE the annotation.
TEST(TypeChecker, AClassBodyAnnotationNarrowsForAReaderAboveIt) {
    expect_clean("class Base:\n"
                 "    v: object\n"
                 "class Child(Base):\n"
                 "    def use(self) -> int:\n"
                 "        return self.v + 1\n"
                 "    v: int\n");
}

// The ANNOTATED self form is a declaration too. Verified: `Success`.
TEST(TypeChecker, AnAnnotatedSelfAttributeNarrowsForAReaderAboveIt) {
    expect_clean("class Base:\n"
                 "    def __init__(self) -> None:\n"
                 "        self.v: object = 1\n"
                 "class Child(Base):\n"
                 "    def use(self) -> int:\n"
                 "        return self.v + 1\n"
                 "    def m(self) -> None:\n"
                 "        self.v: int = 1\n");
}

// THE COUNTERPART that pins the guard split: a PLAIN `self.v = ...` in a
// subclass method is NOT a narrowing declaration. Verified against mypy
// 1.18.1, both halves:
//   - `class Base: v: object` / Child reading `self.v + 1` and separately
//     doing `self.v = 0` still reports
//     `Unsupported operand types for + ("object" and "int")`;
//   - `class Base: v: int` / Child doing `self.v = "s"` still reports
//     `Incompatible types in assignment (expression has type "str", variable
//     has type "int")`.
// So the plain form's placeholder guard must keep walking the base chain. If
// it did not, this test's error would silently disappear.
TEST(TypeChecker, APlainSelfAssignmentDoesNotRedeclareAnInheritedAttribute) {
    const Checked checked = check_module("class Base:\n"
                                         "    v: int = 0\n"
                                         "class Child(Base):\n"
                                         "    def m(self) -> None:\n"
                                         "        self.v = \"s\"\n");
    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message,
              "incompatible types in assignment (expression has type \"str\", "
              "variable has type \"int\")");
}

// Eager collection must not resolve any annotation TWICE -- a bad one would
// be reported once per pass. Exactly one diagnostic is the assertion.
TEST(TypeChecker, ABadClassBodyAnnotationIsReportedExactlyOnce) {
    const Checked checked = check_module("class Bag:\n"
                                         "    v: Nope\n");
    EXPECT_EQ(checked.diagnostics.size(), 1u);
}

TEST(TypeChecker, ABadMethodAnnotationIsReportedExactlyOnce) {
    const Checked checked = check_module("class Bag:\n"
                                         "    def m(self, a: Nope) -> None:\n"
                                         "        pass\n");
    EXPECT_EQ(checked.diagnostics.size(), 1u);
}

// The declared-LINE disambiguator survives eager collection: a placeholder is
// still declared at its own statement's line, so a same-class re-annotation
// is still recognised as a genuine second declaration rather than as the
// first one's own placeholder. Expectation taken from a mypy run on this
// exact source, not from recall (mypy reports `Name "v" already defined on
// line 2 [no-redef]`).
TEST(TypeChecker, ASameClassReAnnotationIsStillARedefinitionAfterEagerCollection) {
    const Checked checked = check_module("class Bag:\n"
                                         "    v: int\n"
                                         "    v: int\n");
    EXPECT_EQ(checked.diagnostics.size(), 1u);
}

// The eager placeholder for an ANNOTATED `self.x: T` carries the RESOLVED
// annotation, not Type::unknown(). Unknown is absorbing, so installing it
// stops every EARLIER `self.x = ...` in the same class from being checked
// against the type the attribute actually has -- and the write above is
// exactly such an earlier one. Verified against mypy 1.18.1 on this exact
// source: `Incompatible types in assignment (expression has type "str",
// variable has type "int")` at the `self.v = "s"` line.
//
// ORDER-DEPENDENT by construction, which is why both orders are pinned: with
// the annotating method written FIRST, the write is checked against an entry
// the ordinary walk has already filled in, so the error survives even an
// Unknown placeholder. It is only the reader-or-writer-ABOVE order that the
// placeholder's own type decides.
TEST(TypeChecker, APlainSelfWriteAboveAnAnnotatedSelfDeclarationIsStillChecked) {
    const Checked checked = check_module("class Base:\n"
                                         "    v: int\n"
                                         "class Child(Base):\n"
                                         "    def a(self) -> None:\n"
                                         "        self.v = \"s\"\n"
                                         "    def b(self) -> None:\n"
                                         "        self.v: int = 1\n");
    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.line, 5);
    EXPECT_EQ(error.message,
              "incompatible types in assignment (expression has type \"str\", "
              "variable has type \"int\")");
}

TEST(TypeChecker, APlainSelfWriteBelowAnAnnotatedSelfDeclarationIsStillChecked) {
    const Checked checked = check_module("class Base:\n"
                                         "    v: int\n"
                                         "class Child(Base):\n"
                                         "    def b(self) -> None:\n"
                                         "        self.v: int = 1\n"
                                         "    def a(self) -> None:\n"
                                         "        self.v = \"s\"\n");
    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.line, 7);
    EXPECT_EQ(error.message,
              "incompatible types in assignment (expression has type \"str\", "
              "variable has type \"int\")");
}

// The same shape with the base declaring through an annotated `self.v: int`
// in its own __init__ rather than a class-body annotation. Verified against
// mypy 1.18.1: the identical message, at the `self.v = "s"` line.
TEST(TypeChecker, APlainSelfWriteIsCheckedAgainstAnInheritedAnnotatedSelfDeclaration) {
    const Checked checked = check_module("class Base:\n"
                                         "    def __init__(self) -> None:\n"
                                         "        self.v: int = 1\n"
                                         "class Child(Base):\n"
                                         "    def a(self) -> None:\n"
                                         "        self.v = \"s\"\n"
                                         "    def b(self) -> None:\n"
                                         "        self.v: int = 1\n");
    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.line, 6);
    EXPECT_EQ(error.message,
              "incompatible types in assignment (expression has type \"str\", "
              "variable has type \"int\")");
}

// READS above the declaration are the other half of what an Unknown
// placeholder swallowed. Verified against mypy 1.18.1 on this exact source:
// `"Thing" has no attribute "nonexistent"` at the reading line.
TEST(TypeChecker, ASelfReadAboveAnAnnotatedSelfDeclarationIsStillChecked) {
    const Checked checked = check_module("class Thing:\n"
                                         "    pass\n"
                                         "class Base:\n"
                                         "    v: Thing\n"
                                         "class Child(Base):\n"
                                         "    def a(self) -> None:\n"
                                         "        self.v.nonexistent()\n"
                                         "    def b(self) -> None:\n"
                                         "        self.v: Thing = Thing()\n");
    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.line, 7);
    EXPECT_EQ(error.message, "\"Thing\" has no attribute \"nonexistent\"");
}

// Resolving the annotated self declaration eagerly must not resolve it
// TWICE. The eager scan caches its resolution and the ordinary walk reuses
// it, so a bad annotation draws exactly one diagnostic. Note the base
// declares the same name, which is precisely the case the eager scan does
// NOT skip (the annotated form declares over an inherited name), so this
// really does take the resolve-and-cache path.
TEST(TypeChecker, ABadAnnotatedSelfAnnotationOverAnInheritedNameIsReportedExactlyOnce) {
    const Checked checked = check_module("class Base:\n"
                                         "    v: int\n"
                                         "class Child(Base):\n"
                                         "    def b(self) -> None:\n"
                                         "        self.v: Nope = 1\n");
    EXPECT_EQ(checked.diagnostics.size(), 1u);
    EXPECT_EQ(checked.diagnostics[0].code, "NameError");
}

// The other half of the cache contract: an annotated self declaration the
// eager scan SKIPS (this class already declares the name, here through a
// class-body annotation) is never cached, so the ordinary walk resolves it
// for the first and only time. Still exactly one diagnostic.
TEST(TypeChecker, ABadAnnotatedSelfAnnotationOverAnOwnNameIsReportedExactlyOnce) {
    const Checked checked = check_module("class Bag:\n"
                                         "    v: int\n"
                                         "    def b(self) -> None:\n"
                                         "        self.v: Nope = 1\n");
    EXPECT_EQ(checked.diagnostics.size(), 1u);
    EXPECT_EQ(checked.diagnostics[0].code, "NameError");
}

// The one input found that reaches the class-body annotation branch's
// "a genuinely earlier same-class declaration" arm: two same-named nested
// classes in one outer class are keyed identically in ClassTable, so the
// second body's annotation finds the first body's install at a different
// line. mypy 1.18.1 rejects this source for the duplicate name
// (`Name "Inner" already defined on line 2`), which this model does not
// report -- so the arm's own diagnostic lands on an already-rejected
// program, never on one mypy accepts.
TEST(TypeChecker, ARepeatedNestedClassBodyAnnotationChecksAgainstTheFirstBodysDeclaration) {
    const Checked checked = check_module("class Outer:\n"
                                         "    class Inner:\n"
                                         "        x: int\n"
                                         "    class Inner:\n"
                                         "        x: str\n");
    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.line, 5);
    EXPECT_EQ(error.message,
              "incompatible types in assignment (expression has type \"int\", "
              "variable has type \"str\")");
}

// A program compiles here only when BOTH mypy --strict and CPython accept it;
// if either rejects it, this compiler must not silently accept it, because a
// compiled script has to produce what the same script run normally produces.
// This is a case where only the second oracle objects. Measured, all three:
//   cythonpp before this change: silent
//   mypy --strict:               Success -- it resolves a forward-declared
//                                base fully, inherited __init__ and all, and
//                                is entirely order-insensitive here
//   CPython:                     NameError: name 'Parent' is not defined,
//                                raised at IMPORT, from the `class Child`
//                                statement itself
// So mypy's silence is a gap in mypy, not a licence to compile: emitting C++
// for a program CPython refuses to import would turn a diagnostic gap into a
// wrong-code bug. This reports the message CPython produces.
TEST(TypeChecker, ABaseDeclaredBelowItsSubclassIsANameError) {
    const Checked checked = check_module("class Child(Parent):\n"
                                         "    pass\n"
                                         "class Parent:\n"
                                         "    pass\n");
    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "NameError");
    EXPECT_EQ(error.message, "name 'Parent' is not defined");
    EXPECT_EQ(error.line, 1);
}

// A base declared ABOVE is of course fine, whether flat or inside an `if`:
// Python executes an `if` block's `class` statement, so the name really is
// bound by the time the subclass statement runs.
TEST(TypeChecker, ABaseDeclaredAboveItsSubclassIsClean) {
    expect_clean("class Parent:\n"
                 "    pass\n"
                 "class Child(Parent):\n"
                 "    pass\n");
}

TEST(TypeChecker, ABaseDeclaredInsideAnIfAboveItsSubclassIsClean) {
    expect_clean("FLAG = True\n"
                 "if FLAG:\n"
                 "    class Parent:\n"
                 "        pass\n"
                 "class Child(Parent):\n"
                 "    pass\n");
}

// A base declared below, inside a LATER `if`, is still the error -- the rule
// is about the source position of the binding statement, not about nesting.
TEST(TypeChecker, ABaseDeclaredInsideALaterIfIsANameError) {
    const Checked checked = check_module("FLAG = True\n"
                                         "class Child(Parent):\n"
                                         "    pass\n"
                                         "if FLAG:\n"
                                         "    class Parent:\n"
                                         "        pass\n");
    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "NameError");
    EXPECT_EQ(error.message, "name 'Parent' is not defined");
}

// A function-local class used as a base inside the SAME function: ordinary
// statement order within the function body.
TEST(TypeChecker, AFunctionLocalBaseDeclaredAboveIsClean) {
    expect_clean("def f() -> None:\n"
                 "    class Local:\n"
                 "        pass\n"
                 "    class Sub(Local):\n"
                 "        pass\n"
                 "    print(Sub())\n");
}

// A function body is a DIFFERENT EXECUTION CONTEXT from the module body: it
// runs when the function is called, by which time every module-level `class`
// statement has already executed, however far below the def it is written.
// Both oracles accept this, so the order check must not fire. Measured:
//   mypy --strict: Success
//   CPython:       runs, printing the Sub instance
TEST(TypeChecker, AFunctionLocalClassMayNameAModuleBaseDeclaredBelowTheFunction) {
    expect_clean("def f() -> None:\n"
                 "    class Sub(P):\n"
                 "        pass\n"
                 "    print(Sub())\n"
                 "class P:\n"
                 "    pass\n"
                 "f()\n");
}

// Same rule, same reason, for a class inside a METHOD body. Measured:
//   mypy --strict: Success
//   CPython:       runs, printing the Sub instance
TEST(TypeChecker, AMethodLocalClassMayNameAModuleBaseDeclaredBelowTheClass) {
    expect_clean("class Holder:\n"
                 "    def make(self) -> None:\n"
                 "        class Sub(P):\n"
                 "            pass\n"
                 "        print(Sub())\n"
                 "class P:\n"
                 "    pass\n"
                 "Holder().make()\n");
}

// The in-function REVERSE order is still caught, and this is what makes the
// exemption above narrow rather than a blanket hole: a function-local class
// is reachable only through the scope-limited alias visit(ClassDef) installs
// when the walk reaches its own statement, so `Local` is not a known class
// yet when `Sub` is validated. Measured, CPython:
//   UnboundLocalError: cannot access local variable 'Local' where it is not
//   associated with a value
// (UnboundLocalError is a NameError subclass; the wording differs from ours,
// the code does not, and the point is that this is not silently compiled.)
TEST(TypeChecker, AFunctionLocalBaseDeclaredBelowInTheSameFunctionIsANameError) {
    const Checked checked = check_module("def f() -> None:\n"
                                         "    class Sub(Local):\n"
                                         "        pass\n"
                                         "    class Local:\n"
                                         "        pass\n"
                                         "    print(Sub())\n"
                                         "f()\n");
    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "NameError");
    EXPECT_EQ(error.message, "name 'Local' is not defined");
    EXPECT_EQ(error.line, 2);
}

// KNOWN MISSES, RECORDED. Base validation inspects BARE-NAME bases only, so a
// dotted base is skipped entirely and these two programs draw nothing. Both
// are errors this compiler does not report, which is the safe direction of the
// two, and neither is unreachable -- each is two lines to write. Measured with
// mypy 1.18.1 and CPython 3.14.2:
//
//   `class D(Outer.Inner)` above `class Outer` -- mypy --strict: "Success: no
//   issues found in 1 source file"; CPython: `NameError: name 'Outer' is not
//   defined` from the `class D(Outer.Inner):` statement itself, caret under
//   `Outer` alone. One oracle rejects, so under the union rule this must not
//   compile, and it does.
//
//   `class D(mod.Thing)` with `mod` bound nowhere -- mypy --strict:
//   `Name "mod" is not defined  [name-defined]` plus `Class cannot subclass
//   "Thing" (has type "Any")  [misc]`; CPython: `NameError: name 'mod' is not
//   defined`. BOTH oracles reject, and this compiler is silent.
//
// Reporting either one requires asking whether a dotted base's ROOT is bound
// in scope, and that question is what dragged base validation behind the name
// pre-binding passes and those passes into control flow, which produced false
// NameErrors on ordinary code both oracles accept. See validate_class_bases.
TEST(TypeChecker, ADottedBaseIsNotResolved) {
    expect_clean("class D(Outer.Inner):\n"
                 "    pass\n"
                 "class Outer:\n"
                 "    class Inner:\n"
                 "        pass\n");
    expect_clean("class D(mod.Thing):\n"
                 "    pass\n");
}

// KNOWN WRONG. THIS TEST RECORDS A BUG, NOT DESIRED BEHAVIOUR. Whoever fixes
// the over-fire it pins should DELETE this test, not make the fix satisfy it.
//
// The program below is accepted by both oracles and must therefore compile,
// but this compiler reports it. Measured (mypy 1.18.1, CPython 3.14.2):
//   mypy --strict: Success: no issues found in 1 source file
//   CPython:       runs, printing the Child instance
//   cythonpp:      NameError: name 'Parent' is not defined, at 4:21
// It is clean at run time because the `class Child(Parent)` statement is
// guarded by `ready`, which is only true on the SECOND iteration -- by which
// point the `class Parent` statement below it has already executed once.
//
// WHAT REPORTS IT is AnnotationResolver, from inside TypeChecker::base_types:
// the declaration pass walks classes in SOURCE order and resolves each one's
// bases as it declares it, so when `Child`'s base is resolved at line 4 the
// name `Parent` is not in ClassTable yet. There is no explicit order rule any
// more -- there was one, comparing declaration lines, and it was deleted once
// the resolver's declaration-time timing subsumed it. The over-fire survived
// the deletion unchanged, because both mechanisms proxy execution order by
// SOURCE POSITION, which is correct only in straight-line code, and a loop
// makes a later line run before an earlier one.
//
// Why it is left in place rather than papered over: every syntactic
// refinement tried relocates the hole instead of closing it -- in particular,
// suppressing the check for two classes in the same loop body would silently
// compile this near-identical program, which CPython kills on its first
// iteration (measured: NameError: name 'Parent' is not defined, and this
// compiler does report it, at 2:17):
//   for i in [1]:
//       class Child(Parent):
//           pass
//       class Parent:
//           pass
//   print(Child())
// Telling the two apart needs reaching-definitions over a control-flow graph,
// and this compiler builds no CFG at all. Reporting is also the safe
// direction of the two: a false NameError is a visible, fixable complaint,
// while the alternative emits C++ for a module CPython refuses to import.
//
// This cannot live in the labelled corpus: the corpus guard correctly refuses
// a "# mypy: clean" sample that expects a NameError, and the "# cpython:
// error" exemption does not apply because CPython accepts this program too.
TEST(TypeChecker, KnownWrongLoopReentryMakesAValidForwardBaseAFalseNameError) {
    const Checked checked = check_module("ready = False\n"
                                         "for i in [1, 2]:\n"
                                         "    if ready:\n"
                                         "        class Child(Parent):\n"
                                         "            pass\n"
                                         "    class Parent:\n"
                                         "        pass\n"
                                         "    ready = True\n"
                                         "print(Child())\n");
    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "NameError");
    EXPECT_EQ(error.message, "name 'Parent' is not defined");
    EXPECT_EQ(error.line, 4);
    EXPECT_EQ(error.column, 21);
}

// A subclass of a PARAMETRIC builtin is nominal, but every inherited member
// is substituted. Verified against mypy 1.18.1: `class IntList(list[int])`
// gives reveal_type(a) `IntList`, reveal_type(a[0]) `builtins.int`,
// reveal_type(x) for `for x in a` `builtins.int`, and
// `b: list[int] = IntList()` clean. Recording the base as a bare name dropped
// the element type, which made the assignment a false TypeError and both the
// subscript and the iteration deferrals.
TEST(TypeChecker, AParametricBuiltinSubclassIsAssignableToItsBase) {
    expect_clean("class IntList(list[int]):\n"
                 "    pass\n"
                 "def f() -> None:\n"
                 "    a = IntList()\n"
                 "    b: list[int] = a\n"
                 "    print(b)\n");
}

TEST(TypeChecker, AParametricBuiltinSubclassSubscriptsAsItsElement) {
    expect_clean("class IntList(list[int]):\n"
                 "    pass\n"
                 "def f() -> int:\n"
                 "    a = IntList()\n"
                 "    return a[0] + 1\n");
}

TEST(TypeChecker, AParametricBuiltinSubclassIteratesAsItsElement) {
    expect_clean("class IntList(list[int]):\n"
                 "    pass\n"
                 "def f() -> int:\n"
                 "    total = 0\n"
                 "    for v in IntList():\n"
                 "        total = total + v\n"
                 "    return total\n");
}

// Two levels: `class B(A)` where `A(list[int])`. Verified clean, with
// `b[0]` an int and `c: list[int] = B()` clean.
TEST(TypeChecker, AParametricBaseSubstitutesThroughTwoLevels) {
    expect_clean("class A(list[int]):\n"
                 "    pass\n"
                 "class B(A):\n"
                 "    pass\n"
                 "def f() -> int:\n"
                 "    b = B()\n"
                 "    c: list[int] = b\n"
                 "    print(c)\n"
                 "    return b[0]\n");
}

// ASSIGNABILITY RUNS ONE WAY ONLY. Verified against mypy 1.18.1:
// `Incompatible types in assignment (expression has type "list[int]",
// variable has type "IntList")`. The base-chain walk is directional, so this
// must stay an error after the change.
TEST(TypeChecker, TheParametricBaseIsNotAssignableToItsSubclass) {
    const Checked checked = check_module("class IntList(list[int]):\n"
                                         "    pass\n"
                                         "def f(xs: list[int]) -> None:\n"
                                         "    a: IntList = xs\n"
                                         "    print(a)\n");
    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message,
              "incompatible types in assignment (expression has type \"list[int]\", "
              "variable has type \"IntList\")");
}

// METHODS STAY DEFERRED, AND THAT IS THE POINT -- it is what keeps this
// change cheap: no typeshed, no generic method substitution. `a.append`
// misses in the class table (the subclass declares no such member) and the
// base is a KIND, not a class, so it lands on the existing
// "methods on builtin types" deferral. What matters is that a false
// TypeError became a NotImplementedError, not that the call works.
TEST(TypeChecker, AnInheritedMethodOnAParametricBaseIsDeferred) {
    const Checked checked = check_module("class IntList(list[int]):\n"
                                         "    pass\n"
                                         "def f() -> None:\n"
                                         "    IntList().append(1)\n");
    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "NotImplementedError");
}

// A `tuple[...]` BASE IS DEFERRED, deliberately, and this is the second place
// this compiler diverges from mypy on purpose. Measured: mypy accepts
// `class MyPair(tuple[int, str])` and reveals `MyPair()[0]` as
// `builtins.int` -- but at runtime `MyPair()` is `()`, `len()` is 0 and
// `MyPair()[0]` raises IndexError. mypy models a tuple subclass as a tuple
// type with a nominal fallback and never checks that construction produces
// the claimed arity. A compiler emitting real code must not follow that, so
// this is a NAMED deferral rather than a guess.
TEST(TypeChecker, ATupleBaseIsNotImplemented) {
    const Checked checked = check_module("class MyPair(tuple[int, str]):\n"
                                         "    pass\n");
    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "NotImplementedError");
    EXPECT_EQ(error.message, "a tuple base class is not supported");
}

// ...and the class is still DECLARED, tuple base and all, so a literal index
// through an instance of it resolves ELEMENT-WISE rather than falling back to
// the union. The deferral above covers the base DECLARATION only. Verified
// against mypy 1.18.1 on `p = MyPair((1, "a"))`: reveal_type(p[0]) is
// `builtins.int` and reveal_type(p[1]) is `builtins.str`, and the whole
// program is mypy-clean and prints `1 a` under CPython.
//
// Pinned as "exactly ONE diagnostic": if the base were dropped at
// declaration, `p[0]` would land on the user-class arm and add a second
// NotImplementedError, and if the literal index were declined, `n: int =
// p[0]` would add a TypeError naming `int | str`.
TEST(TypeChecker, ALiteralIndexThroughATupleBaseSelectsOneElement) {
    const Checked checked = check_module("class MyPair(tuple[int, str]):\n"
                                         "    pass\n"
                                         "def f() -> None:\n"
                                         "    p = MyPair()\n"
                                         "    n: int = p[0]\n"
                                         "    s: str = p[1]\n"
                                         "    print(n, s)\n");
    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.message, "a tuple base class is not supported");
}

// The same shape with the elements SWAPPED draws the assignment error, which
// is what makes the test above mean `int` and `str` specifically rather than
// merely "something assignable".
TEST(TypeChecker, AMismatchedLiteralIndexThroughATupleBaseIsReported) {
    const Checked checked = check_module("class MyPair(tuple[int, str]):\n"
                                         "    pass\n"
                                         "def f() -> None:\n"
                                         "    p = MyPair()\n"
                                         "    s: str = p[0]\n"
                                         "    print(s)\n");
    ASSERT_EQ(checked.diagnostics.size(), 2u);
    EXPECT_EQ(checked.diagnostics.back().code, "TypeError");
    EXPECT_EQ(checked.diagnostics.back().message,
              "incompatible types in assignment (expression has type \"int\", "
              "variable has type \"str\")");
}

// A BARE generic base is a real mypy error, so reporting is invariant-safe.
// Verified: `Missing type parameters for generic type "list" [type-arg]`.
// The exact code and wording come from whatever AnnotationResolver already
// produces for a bare `list` annotation.
TEST(TypeChecker, ABareGenericBaseIsReported) {
    const Checked checked = check_module("class L(list):\n"
                                         "    pass\n");
    EXPECT_FALSE(checked.diagnostics.empty());
}

// Unchanged: a NON-parametric builtin base still works, and a bad base name
// is still a NameError.
TEST(TypeChecker, ANonParametricBuiltinBaseStillWorks) {
    expect_clean("class Sub(int):\n"
                 "    pass\n"
                 "x: int = Sub()\n"
                 "print(x)\n");
}

// Subscripting a subclass of a NON-parametric builtin resolves through the
// inherited base, exactly as the parametric cases above do -- it is the same
// widened rule, not a parametric-only one, and it moved too: this used to be
// a NotImplementedError deferral.
//
// Verified against mypy 1.18.1: for `class C(str)` and `c = C()`,
// reveal_type(c) is "C" and reveal_type(c[0]) is "builtins.str", and the
// program below is "Success: no issues found in 1 source file". So
// `s: str = c[0]` must be accepted, and the deferral this used to produce was
// a NotImplementedError on a program mypy takes.
//
// The body is deliberately never CALLED here. Calling it would index the
// EMPTY string `C()` returns and raise IndexError under CPython (measured) --
// a runtime data condition, nothing to do with the rule under test.
// (`C("abc")` is a separate question -- the inherited builtin overload set
// this model cannot spell, see
// TypeChecker.ABuiltinBasedSubclassDefersItsConstructorArguments -- but the
// empty-string IndexError concern stands on its own regardless.)
TEST(TypeChecker, ANonParametricBuiltinSubclassSubscriptsAsItsBaseElement) {
    expect_clean("class C(str):\n"
                 "    pass\n"
                 "def f() -> None:\n"
                 "    c = C()\n"
                 "    s: str = c[0]\n"
                 "    print(s)\n");
}

// Verified against mypy 1.18.1: `Success`.
TEST(TypeChecker, AConstantTupleIndexSelectsTheElementType) {
    expect_clean("def f() -> None:\n"
                 "    t: tuple[int, str] = (1, \"a\")\n"
                 "    a: int = t[0]\n"
                 "    b: str = t[1]\n"
                 "    c: str = t[-1]\n"
                 "    print(a, b, c)\n");
}

// Verified against mypy 1.18.1: `Tuple index out of range [misc]`.
TEST(TypeChecker, AnOutOfRangeConstantTupleIndexIsReported) {
    const Checked checked = check_module("def f() -> None:\n"
                                         "    t: tuple[int, str] = (1, \"a\")\n"
                                         "    print(t[5])\n");
    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "tuple index out of range");
}

// A VARIABLE index still yields the union, so this stays a genuine error --
// and mypy agrees, since it gives `builtins.int | builtins.str` there too.
TEST(TypeChecker, AVariableTupleIndexStillYieldsTheUnion) {
    const Checked checked = check_module("def f(i: int) -> None:\n"
                                         "    t: tuple[int, str] = (1, \"a\")\n"
                                         "    a: int = t[i]\n"
                                         "    print(a)\n");
    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message,
              "incompatible types in assignment (expression has type \"int | str\", "
              "variable has type \"int\")");
}

// A bare builtin FUNCTION used as a VALUE. Verified against mypy 1.18.1:
// `Success`, with reveal_type(g) `def (typing.Sized) -> builtins.int`. This
// was `NameError: name 'len' is not defined` -- a false NameError, and the
// smallest known violation of the hard invariant before this change.
TEST(TypeChecker, ABuiltinFunctionUsedAsAValueIsClean) {
    expect_clean("def f() -> None:\n"
                 "    g = len\n"
                 "    print(g)\n");
}

TEST(TypeChecker, ABuiltinFunctionIsAssignableToObject) {
    expect_clean("x: object = len\n"
                 "print(x)\n");
}

// THE SAME STRUCTURAL GAP, wider than a bare value: a CALL to a builtin
// function this model does not model was a false NameError too. Verified:
// `print(hash(x))` is `Success` under mypy --strict. It becomes the existing
// "not supported" deferral, matching how `zip(...)` is already handled --
// never NameError, since the name IS defined and mypy accepts the call.
TEST(TypeChecker, ACallToAnUnmodelledBuiltinFunctionIsDeferred) {
    const Checked checked = check_module("def f(x: int) -> None:\n"
                                         "    print(hash(x))\n");
    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "NotImplementedError");
    EXPECT_EQ(error.message, "calls to builtin 'hash' are not supported");
}

// PRECEDENCE, and it must stay this way: a live scope binding of the same
// spelling wins, because the carve-out sits inside type_of_name's
// "resolution came back null" branch. `def f(len: str) -> int: return 1` uses
// the parameter.
TEST(TypeChecker, ALocalNamedLikeABuiltinFunctionStillWins) {
    const Checked checked = check_module("def f(len: str) -> int:\n"
                                         "    return len\n");
    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
}

// A USER class of the same spelling wins too -- the class carve-out is
// checked BEFORE the function table.
TEST(TypeChecker, AUserClassNamedLikeABuiltinFunctionStillWins) {
    expect_clean("class hash:\n"
                 "    def __init__(self) -> None:\n"
                 "        self.n = 0\n"
                 "def f() -> int:\n"
                 "    h = hash()\n"
                 "    return h.n\n");
}

// THE ASYMMETRIC CELL: this precedence does NOT extend to a builtin-function
// name that is ALSO one of the modelled kSupportedBuiltinCalls entries (15 of
// the 49 function-table names, `len` among them) -- is_supported_builtin_call
// short-circuits earlier in type_of_name_call, before the user-class-wins
// condition is ever consulted, so `class len` still resolves as the modelled
// builtin call rather than the user's own constructor. Verified: mypy
// --strict reports `Success` and CPython prints `0` for the equivalent
// top-level program, so this is a MISSED error, not a false one --
// NotImplementedError keeps it inside the sanctioned escape hatch. Pinned
// here so the asymmetry with the `hash` case just above stays a recorded
// fact rather than folklore.
TEST(TypeChecker, AUserClassNamedLikeAModelledBuiltinCallDoesNotWin) {
    const Checked checked = check_module("class len:\n"
                                         "    def __init__(self) -> None:\n"
                                         "        self.n = 0\n"
                                         "def f() -> int:\n"
                                         "    h = len()\n"
                                         "    return h.n\n");
    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "NotImplementedError");
    EXPECT_EQ(error.message,
              "calls to builtin 'len' with these argument types are not supported");
}

// An ordinary unbound name is STILL a NameError. This table must not turn
// every misspelling into silence.
TEST(TypeChecker, AnUnknownNameIsStillANameError) {
    const Checked checked = check_module("def f() -> None:\n"
                                         "    print(nope)\n");
    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "NameError");
    EXPECT_EQ(error.message, "name 'nope' is not defined");
}

// Verified against mypy 1.18.1: `Success` -- assigning a narrower type to a
// path makes the next READ of that path see the narrowed type.
TEST(TypeChecker, AnAssignmentNarrowsASelfAttributeForLaterReads) {
    expect_clean("class Bag:\n"
                 "    n: object = object()\n"
                 "    def m(self) -> None:\n"
                 "        self.n = 7\n"
                 "        print(self.n + 1)\n");
}

// `self` is not special. Verified: mypy narrows `b.n` for a plain parameter
// exactly as it narrows `self.n`.
TEST(TypeChecker, AnAssignmentNarrowsAPlainParametersAttribute) {
    expect_clean("class Bag:\n"
                 "    n: object = object()\n"
                 "def m(b: Bag) -> None:\n"
                 "    b.n = 7\n"
                 "    print(b.n + 1)\n");
}

// A plain local narrows too, when its DECLARED type is wider than what was
// assigned.
TEST(TypeChecker, AnAssignmentNarrowsALocalWithAWiderAnnotation) {
    expect_clean("def f() -> None:\n"
                 "    x: object = object()\n"
                 "    x = 7\n"
                 "    print(x + 1)\n");
}

// A NESTED PATH narrows, and only the path assigned.
TEST(TypeChecker, ANestedPathNarrowsIndependentlyOfItsSiblings) {
    expect_clean("class Inner:\n"
                 "    v: object = object()\n"
                 "class Outer:\n"
                 "    def __init__(self) -> None:\n"
                 "        self.inner = Inner()\n"
                 "    def m(self) -> None:\n"
                 "        self.inner.v = 7\n"
                 "        print(self.inner.v + 1)\n");
}

// KILL ON PREFIX: reassigning the receiver invalidates the narrowing on
// everything under it, so the read falls back to the declared type and the
// arithmetic is a genuine error. Confirmed against mypy 1.18.1: reported.
TEST(TypeChecker, ReassigningAReceiverInvalidatesNarrowingsBeneathIt) {
    const Checked checked = check_module("class Inner:\n"
                                         "    v: object = object()\n"
                                         "class Outer:\n"
                                         "    def __init__(self) -> None:\n"
                                         "        self.inner = Inner()\n"
                                         "    def m(self) -> None:\n"
                                         "        self.inner.v = 7\n"
                                         "        self.inner = Inner()\n"
                                         "        print(self.inner.v + 1)\n");
    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
}

// THE DECLARED TYPE IS A PERMANENT CEILING: narrowing never widens what may
// be assigned next. `self.n` narrowed to int still accepts a str, because its
// DECLARED type is object. Verified clean under mypy.
TEST(TypeChecker, NarrowingDoesNotRestrictWhatMayBeAssignedNext) {
    expect_clean("class Bag:\n"
                 "    n: object = object()\n"
                 "    def m(self) -> None:\n"
                 "        self.n = 7\n"
                 "        self.n = \"s\"\n"
                 "        print(self.n)\n");
}

// NO CALL INVALIDATION -- the measurement that determines the whole design.
// Verified against mypy 1.18.1: this reveals `builtins.int` even though
// `other()` provably assigns a str to the same attribute. Narrowing survives
// every call. Being SOUND here would mean rejecting a program mypy accepts.
TEST(TypeChecker, NarrowingSurvivesACallThatInvalidatesIt) {
    expect_clean("class Bag:\n"
                 "    n: object = object()\n"
                 "    def other(self) -> None:\n"
                 "        self.n = \"reset\"\n"
                 "    def m(self) -> None:\n"
                 "        self.n = 7\n"
                 "        self.other()\n"
                 "        print(self.n + 1)\n");
}

// RESET AT EVERY FUNCTION BOUNDARY, and this is the ONE wall where getting it
// wrong produces a FALSE NEGATIVE. Verified against mypy 1.18.1: the nested
// def reveals `builtins.object` and `self.n + 1` there is a genuine
// `Unsupported operand types for + ("object" and "int")`.
TEST(TypeChecker, NarrowingResetsInsideANestedDef) {
    const Checked checked = check_module("class Bag:\n"
                                         "    n: object = object()\n"
                                         "    def m(self) -> None:\n"
                                         "        self.n = 7\n"
                                         "        def inner() -> None:\n"
                                         "            print(self.n + 1)\n"
                                         "        inner()\n");
    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message,
              "unsupported operand types for + (\"object\" and \"int\")");
}

// Another method of the same class sees the declared type too.
TEST(TypeChecker, NarrowingDoesNotLeakIntoAnotherMethod) {
    const Checked checked = check_module("class Bag:\n"
                                         "    n: object = object()\n"
                                         "    def m(self) -> None:\n"
                                         "        self.n = 7\n"
                                         "    def p(self) -> None:\n"
                                         "        print(self.n + 1)\n");
    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
}

// DO NOT FOLLOW ALIASES. Verified against mypy 1.18.1: `b = self; b.n = 7`
// reveals `self.n` as `builtins.object` and `b.n` as `builtins.int`, so
// reading `self.n` arithmetically here IS an error and reading `b.n` is not.
TEST(TypeChecker, NarrowingDoesNotFollowAnAlias) {
    const Checked checked = check_module("class Bag:\n"
                                         "    n: object = object()\n"
                                         "    def m(self) -> None:\n"
                                         "        b = self\n"
                                         "        b.n = 7\n"
                                         "        print(b.n + 1)\n"
                                         "        print(self.n + 1)\n");
    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.line, 7);
}

// A FAILED assignment leaves the entry untouched: the incompatible
// assignment is reported and nothing is narrowed to a type the declared type
// forbids.
TEST(TypeChecker, AnIncompatibleAssignmentDoesNotNarrow) {
    const Checked checked = check_module("class Bag:\n"
                                         "    n: int = 0\n"
                                         "    def m(self) -> None:\n"
                                         "        self.n = \"s\"\n"
                                         "        print(self.n + 1)\n");
    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.line, 4);
}

// A NAME target's own declaring AnnAssign does NOT narrow from its value --
// verified against mypy 1.18.1: `x: object = 5` reveals `builtins.object` on
// the very next line, not `builtins.int`, and `x + 1` there is a genuine
// "Unsupported operand types" error. Narrowing a plain variable only ever
// comes from a SEPARATE, later plain reassignment (assign_name's own rule),
// never from the declaring annotation itself -- this is the asymmetry
// redeclare_narrowing's Name-target branch exists to get right.
TEST(TypeChecker, AnAnnAssignDoesNotNarrowANameFromItsOwnDeclaringValue) {
    const Checked checked = check_module("def f() -> None:\n"
                                         "    x: object = 5\n"
                                         "    print(x + 1)\n");
    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
}

// An ATTRIBUTE target's own declaring AnnAssign DOES narrow from its value,
// even for a BRAND NEW attribute never declared before -- verified against
// mypy 1.18.1: `self.n: object = 5` (n not previously declared) reveals
// `builtins.int` on the next line and is Success, matching how the plain
// `self.n = 5` form already narrows.
TEST(TypeChecker, AnAnnAssignNarrowsABrandNewSelfAttributeFromItsOwnDeclaringValue) {
    expect_clean("class Bag:\n"
                 "    def m(self) -> None:\n"
                 "        self.n: object = 5\n"
                 "        print(self.n + 1)\n");
}

// A COMPREHENSION TARGET SHADOWS AN ENCLOSING NARROWING OF THE SAME NAME.
// A narrowing key carries no scope, and a comprehension is deliberately NOT
// a boundary, so the comprehension's own `x` would otherwise read the
// enclosing `x`'s narrowing. Verified against mypy 1.18.1: this is
// `Unsupported operand types for + ("str" and "int")`, and CPython raises
// `TypeError: can only concatenate str (not "int") to str` on the same file,
// so BOTH oracles reject it -- reading the enclosing narrowing here is a
// silent accept, the one direction this compiler must never take.
TEST(TypeChecker, AComprehensionTargetShadowsAnEnclosingNarrowingOfTheSameName) {
    const Checked checked = check_module("def f() -> None:\n"
                                         "    x: object = object()\n"
                                         "    x = 7\n"
                                         "    ys = [x + 1 for x in [\"a\", \"b\"]]\n"
                                         "    print(ys)\n");
    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "unsupported operand types for + (\"str\" and \"int\")");
}

// The ATTRIBUTE flavour, which the bare-name shadow must cover for free:
// killing `b` also kills `b.n`, per NarrowingMap::kill's prefix rule, so the
// comprehension's fresh `b` reads `n`'s DECLARED type. Verified against mypy
// 1.18.1 (`Unsupported operand types for + ("object" and "int")`) and
// CPython (`TypeError: unsupported operand type(s) for +: 'object' and
// 'int'`).
TEST(TypeChecker, AComprehensionTargetShadowsANarrowedAttributeRoot) {
    const Checked checked = check_module("class Bag:\n"
                                         "    n: object = object()\n"
                                         "def f(items: list[Bag], b: Bag) -> None:\n"
                                         "    b.n = 7\n"
                                         "    ys = [b.n + 1 for b in items]\n"
                                         "    print(ys)\n");
    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "unsupported operand types for + (\"object\" and \"int\")");
}

// The same at MODULE scope, where the narrowing lives in the module's own
// scope rather than a function's -- the shadow is a property of the key
// having no scope at all, so it must not depend on which scope narrowed.
// Same two oracle verdicts as the function-scope case above.
TEST(TypeChecker, AComprehensionTargetShadowsAModuleScopeNarrowing) {
    const Checked checked = check_module("x: object = object()\n"
                                         "x = 7\n"
                                         "ys = [x + 1 for x in [\"a\", \"b\"]]\n"
                                         "print(ys)\n");
    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "unsupported operand types for + (\"str\" and \"int\")");
}

// NESTED: the read sits in an INNER comprehension, one more scope down from
// the shadowing target, so the shadow has to outlive the clause that
// installed it and still be visible to a nested comprehension's own guard.
// Same two oracle verdicts again.
TEST(TypeChecker, AnInnerComprehensionStillSeesAnOuterTargetsShadow) {
    const Checked checked =
        check_module("def f() -> None:\n"
                     "    x: object = object()\n"
                     "    x = 7\n"
                     "    ys = [[x + 1 for _ in range(2)] for x in [\"a\", \"b\"]]\n"
                     "    print(ys)\n");
    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "unsupported operand types for + (\"str\" and \"int\")");
}

// SHADOWED, NOT DESTROYED: the enclosing narrowing comes back after the
// comprehension ends. Verified against mypy 1.18.1: `[str(x) for x in
// ["a", "b"]]` followed by `reveal_type(x)` reveals `builtins.int`, the file
// is Success, and CPython prints `8`. A permanent kill instead of a scoped
// one would invent a false TypeError here.
TEST(TypeChecker, AnEnclosingNarrowingSurvivesPastAComprehension) {
    expect_clean("def f() -> None:\n"
                 "    x: object = object()\n"
                 "    x = 7\n"
                 "    ys = [str(x) for x in [\"a\", \"b\"]]\n"
                 "    print(x + 1)\n"
                 "    print(ys)\n");
}

// A PARAMETER DEFAULT IS CHECKED AGAINST THE DECLARED TYPE, NOT THE NARROWED
// ONE, even though the narrowing is live on the line immediately above.
// Verified against mypy 1.18.1: `reveal_type(x)` on the preceding line is
// `builtins.int` while the `def` line is `Incompatible default for argument
// "a" (default has type "object", argument has type "int")`. CPython runs
// the file and prints `7`, so this is mypy alone rejecting -- which the
// union rule still covers, and accepting it silently is a false negative.
TEST(TypeChecker, AParameterDefaultIsCheckedAgainstTheDeclaredTypeNotTheNarrowedOne) {
    const Checked checked = check_module("def f() -> None:\n"
                                         "    x: object = object()\n"
                                         "    x = 7\n"
                                         "    def inner(a: int = x) -> None:\n"
                                         "        print(a)\n"
                                         "    inner()\n");
    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "incompatible default for argument \"a\" (default has "
                             "type \"object\", argument has type \"int\")");
}

// The same at MODULE scope, and with `str` rather than `int` as the
// parameter type -- measured separately so the rule is not mistaken for a
// quirk of one scope or one type. mypy 1.18.1 reports the same
// `Incompatible default` in both, and CPython runs the module-scope one.
TEST(TypeChecker, AParameterDefaultIgnoresAModuleScopeNarrowing) {
    const Checked checked = check_module("x: object = object()\n"
                                         "x = 7\n"
                                         "def inner(a: int = x) -> None:\n"
                                         "    print(a)\n"
                                         "inner()\n");
    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "incompatible default for argument \"a\" (default has "
                             "type \"object\", argument has type \"int\")");
}

TEST(TypeChecker, AParameterDefaultIgnoresANarrowingToStr) {
    const Checked checked = check_module("def f() -> None:\n"
                                         "    x: object = object()\n"
                                         "    x = \"s\"\n"
                                         "    def inner(a: str = x) -> None:\n"
                                         "        print(a)\n"
                                         "    inner()\n");
    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "incompatible default for argument \"a\" (default has "
                             "type \"object\", argument has type \"str\")");
}

// A GENUINE TWO-BRANCH SPLIT gives a real union, not a widen. Verified
// against mypy 1.18.1: `builtins.int | builtins.str`. So the arithmetic
// below is a real error (mypy reports it too) while `object` accepts it.
TEST(TypeChecker, TwoBranchesJoinToTheirUnion) {
    expect_clean("class Bag:\n"
                 "    n: object = object()\n"
                 "    def m(self, f: bool) -> None:\n"
                 "        if f:\n"
                 "            self.n = 7\n"
                 "        else:\n"
                 "            self.n = \"s\"\n"
                 "        x: object = self.n\n"
                 "        print(x)\n");
}

// TwoBranchesJoinToTheirUnion above only reads the joined path through a
// wider `object` target, which stays clean whether the join keeps both
// branches' contributions or just one -- so it cannot, by itself, catch a
// join that silently dropped the `if`-branch and kept only the `else` edge.
// This applies `+` directly to the joined path instead: a Union operand
// defers every operator (NotImplementedError, distinct from any TypeError a
// single-branch narrowing would produce), so this fails differently -- or
// not at all -- if a branch's contribution goes missing from the join.
TEST(TypeChecker, TwoBranchesJoinDefersOnTheUnionRatherThanJustOneEdge) {
    const Checked checked = check_module("class Bag:\n"
                                         "    n: object = object()\n"
                                         "    def m(self, f: bool) -> None:\n"
                                         "        if f:\n"
                                         "            self.n = 7\n"
                                         "        else:\n"
                                         "            self.n = \"s\"\n"
                                         "        print(self.n + 1)\n");
    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "NotImplementedError");
    EXPECT_EQ(error.message, "operations on a union-typed value require "
                             "narrowing, which is not supported");
}

// AGREEING BRANCHES COLLAPSE, because the join de-duplicates. Verified:
// `builtins.int`, so `self.n + 1` afterwards is clean.
TEST(TypeChecker, AgreeingBranchesJoinToOneType) {
    expect_clean("class Bag:\n"
                 "    n: object = object()\n"
                 "    def m(self, f: bool) -> int:\n"
                 "        if f:\n"
                 "            self.n = 7\n"
                 "        else:\n"
                 "            self.n = 8\n"
                 "        return self.n + 1\n");
}

// AN `if` WITH NO `else` WIDENS BACK TO THE DECLARED TYPE, because the
// fall-through edge contributes it. Verified against mypy 1.18.1:
// `builtins.object`, and `self.n + 1` there IS
// `Unsupported operand types for + ("object" and "int")`. Without the join
// this leaked the branch's `int` past the `if` and silently accepted it.
TEST(TypeChecker, AnIfWithoutAnElseWidensBackToTheDeclaredType) {
    const Checked checked = check_module("class Bag:\n"
                                         "    n: object = object()\n"
                                         "    def m(self, f: bool) -> int:\n"
                                         "        if f:\n"
                                         "            self.n = 7\n"
                                         "        return self.n + 1\n");
    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message,
              "unsupported operand types for + (\"object\" and \"int\")");
}

// A NARROWING ESTABLISHED BEFORE A LOOP REACHES INTO THE BODY and past it
// when the body assigns nothing. Verified: `builtins.int` after the loop.
TEST(TypeChecker, ANarrowingSurvivesALoopThatAssignsNothing) {
    expect_clean("class Bag:\n"
                 "    n: object = object()\n"
                 "    def m(self, xs: list[int]) -> int:\n"
                 "        self.n = 7\n"
                 "        for x in xs:\n"
                 "            print(x)\n"
                 "        return self.n + 1\n");
}

TEST(TypeChecker, ANarrowingReachesIntoALoopBody) {
    expect_clean("class Bag:\n"
                 "    n: object = object()\n"
                 "    def m(self, xs: list[int]) -> None:\n"
                 "        self.n = 7\n"
                 "        for x in xs:\n"
                 "            print(self.n + x)\n");
}

// AFTER A LOOP WHOSE BODY ASSIGNS, the state is the union of the pre-loop
// state and the end-of-body state. Verified: `builtins.int | builtins.str`,
// so the arithmetic is an error and an `object` target is clean.
TEST(TypeChecker, ALoopBodysAssignmentJoinsIntoThePostLoopState) {
    expect_clean("class Bag:\n"
                 "    n: object = object()\n"
                 "    def m(self, xs: list[int]) -> None:\n"
                 "        self.n = 7\n"
                 "        for x in xs:\n"
                 "            self.n = \"s\"\n"
                 "        y: object = self.n\n"
                 "        print(y)\n");
}

TEST(TypeChecker, ThePostLoopUnionIsNotStillTheNarrowedType) {
    const Checked checked = check_module("class Bag:\n"
                                         "    n: object = object()\n"
                                         "    def m(self, xs: list[int]) -> int:\n"
                                         "        self.n = 7\n"
                                         "        for x in xs:\n"
                                         "            self.n = \"s\"\n"
                                         "        return self.n + 1\n");
    EXPECT_FALSE(checked.diagnostics.empty());
}

// ThePostLoopUnionIsNotStillTheNarrowedType above only asserts that SOME
// diagnostic appears, which a join that dropped the pre-loop edge entirely
// (keeping just the end-of-body state) would also produce -- `self.n + 1`
// on a bare `str` is its own TypeError. This pins the diagnostic's CODE
// instead: the correct join is `int | str` (pre-loop `int` union end-of-body
// `str`), and a Union operand defers every operator
// (NotImplementedError), which a single-branch `str` narrowing would not.
TEST(TypeChecker, ThePostLoopJoinIncludesThePreLoopEdgeNotJustEndOfBody) {
    const Checked checked = check_module("class Bag:\n"
                                         "    n: object = object()\n"
                                         "    def m(self, xs: list[int]) -> int:\n"
                                         "        self.n = 7\n"
                                         "        for x in xs:\n"
                                         "            self.n = \"s\"\n"
                                         "        return self.n + 1\n");
    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "NotImplementedError");
    EXPECT_EQ(error.message, "operations on a union-typed value require "
                             "narrowing, which is not supported");
}

// THE MEASURED, ACCEPTED DIVERGENCE, pinned as a test so it cannot change
// silently. At the LOOP HEAD mypy sees the union fixpoint
// (`builtins.int | builtins.str`, so `self.n + 1` there is an error it
// reports) while this checker walks the body ONCE and therefore sees the
// pre-loop narrowing (`int`, so it is clean). A MISSED error, which is the
// invariant-safe direction. The two-pass alternative would re-run every
// side effect of the body walk -- every diagnostic reported, every scope bind,
// every member declaration -- and double-report all of them; the other
// single-pass option, widening on loop entry, would instead REJECT the
// mypy-clean `self.n = 7` / `while cond: print(self.n + 1); self.n = 8`.
TEST(TypeChecker, ALoopHeadSeesThePreLoopNarrowingRatherThanTheFixpoint) {
    expect_clean("class Bag:\n"
                 "    n: object = object()\n"
                 "    def m(self, xs: list[int]) -> None:\n"
                 "        self.n = 7\n"
                 "        for x in xs:\n"
                 "            print(self.n + 1)\n"
                 "            self.n = \"s\"\n");
}

// A COMPATIBLE loop body must stay clean -- this is the case that rules out
// widening on loop entry. Verified against mypy 1.18.1: `Success`.
TEST(TypeChecker, ALoopBodyAssigningACompatibleTypeStaysClean) {
    expect_clean("class Bag:\n"
                 "    n: object = object()\n"
                 "    def m(self, f: bool) -> None:\n"
                 "        self.n = 7\n"
                 "        while f:\n"
                 "            print(self.n + 1)\n"
                 "            self.n = 8\n");
}

// A branch's narrowing must not leak into the SIBLING branch: each starts
// from the pre-`if` state.
TEST(TypeChecker, ABranchsNarrowingDoesNotLeakIntoItsSibling) {
    const Checked checked = check_module("class Bag:\n"
                                         "    n: object = object()\n"
                                         "    def m(self, f: bool) -> None:\n"
                                         "        if f:\n"
                                         "            self.n = 7\n"
                                         "        else:\n"
                                         "            print(self.n + 1)\n");
    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.line, 7);
}

// THE `classes_` ARGUMENT MUST BE THREADED THROUGH the join, not left null.
// A joined `Sub | Base` against a path declared `Base` is equivalent to
// `Base` only when the base chain is resolvable -- with `classes` null,
// `is_equivalent` cannot see that `Sub` is a `Base`, so the join keeps a
// stored `Union`, and applying `+` to a Union operand is
// UnsupportedReason::UnionOperand ("operations on a union-typed value
// require narrowing, which is not supported"). Passing `&classes_`
// recognises the union as equivalent to the declared `Base` and drops it, so
// the same `+` instead hits the ordinary user-class-operand path,
// UnsupportedReason::UserClassOperator ("operators on user-defined class
// instances are not supported") -- still a NotImplementedError either way
// (neither Base nor a Base-or-Sub union models `+`), but a DIFFERENT reason,
// which is what makes this test able to tell the two code paths apart.
TEST(TypeChecker, AJoinRecognisesASubclassAsEquivalentToItsDeclaredBase) {
    const Checked checked = check_module("class Base:\n"
                                         "    pass\n"
                                         "class Sub(Base):\n"
                                         "    pass\n"
                                         "class Bag:\n"
                                         "    n: Base = Base()\n"
                                         "    def m(self, f: bool) -> None:\n"
                                         "        if f:\n"
                                         "            self.n = Sub()\n"
                                         "        print(self.n + 1)\n");
    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "NotImplementedError");
    EXPECT_EQ(error.message,
              "operators on user-defined class instances are not supported");
}

// A BRANCH THAT ALWAYS RETURNS MUST NOT CONTRIBUTE ITS EDGE. Its
// end-of-branch state is unreachable at the merge, and taking it anyway
// widens the surviving branch's narrowing back to the declared type -- a
// FALSE TypeError on a program both oracles accept.
//
// Verified against mypy 1.18.1 (`Success`) and CPython 3.13.5 (prints `8`
// then `0`) for all four shapes below: the narrowed path as a `self.`
// attribute and as a plain local, each with the returning branch written
// second and first.
TEST(TypeChecker, AReturningElseBranchContributesNoEdgeToTheJoinForAnAttribute) {
    expect_clean("class Bag:\n"
                 "    n: object = object()\n"
                 "    def m(self, f: bool) -> int:\n"
                 "        if f:\n"
                 "            self.n = 7\n"
                 "        else:\n"
                 "            return 0\n"
                 "        return self.n + 1\n");
}

TEST(TypeChecker, AReturningIfBranchContributesNoEdgeToTheJoinForAnAttribute) {
    expect_clean("class Bag:\n"
                 "    n: object = object()\n"
                 "    def m(self, f: bool) -> int:\n"
                 "        if not f:\n"
                 "            return 0\n"
                 "        else:\n"
                 "            self.n = 7\n"
                 "        return self.n + 1\n");
}

TEST(TypeChecker, AReturningElseBranchContributesNoEdgeToTheJoinForALocal) {
    expect_clean("def m(f: bool) -> int:\n"
                 "    n: object = object()\n"
                 "    if f:\n"
                 "        n = 7\n"
                 "    else:\n"
                 "        return 0\n"
                 "    return n + 1\n");
}

TEST(TypeChecker, AReturningIfBranchContributesNoEdgeToTheJoinForALocal) {
    expect_clean("def m(f: bool) -> int:\n"
                 "    n: object = object()\n"
                 "    if not f:\n"
                 "        return 0\n"
                 "    else:\n"
                 "        n = 7\n"
                 "    return n + 1\n");
}

// THE OTHER HALF OF THE SAME RULE, and the one that keeps the four tests
// above from being satisfiable by simply dropping the `else` edge always: a
// NON-terminating `else` still contributes. Asserts the exact code and
// message, because a join that kept only the if-edge would report a
// `TypeError` on `int + 1`... i.e. nothing at all, and a join that kept only
// the else-edge would report `TypeError` on `str + 1` -- neither is the
// `int | str` union this must produce. Verified against mypy 1.18.1:
// `Unsupported operand types for + ("str" and "int")`, left operand
// `int | str`.
TEST(TypeChecker, ANonReturningElseBranchStillContributesItsEdge) {
    const Checked checked = check_module("class Bag:\n"
                                         "    n: object = object()\n"
                                         "    def m(self, f: bool) -> int:\n"
                                         "        if f:\n"
                                         "            self.n = 7\n"
                                         "        else:\n"
                                         "            self.n = \"s\"\n"
                                         "        return self.n + 1\n");
    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "NotImplementedError");
    EXPECT_EQ(error.message, "operations on a union-typed value require "
                             "narrowing, which is not supported");
    EXPECT_EQ(error.line, 8);
}

// When BOTH branches return, dropping both edges would leave the join with
// nothing to union. The merge is unreachable in that case, so whatever state
// it carries is never read -- keeping both edges is the cheap, safe
// fallback, and this pins that it neither crashes nor invents a diagnostic.
// Verified against mypy 1.18.1: `Success`; CPython 3.13.5 prints `1`.
TEST(TypeChecker, BothBranchesReturningKeepsBothEdgesRatherThanNone) {
    expect_clean("def m(f: bool) -> int:\n"
                 "    n: object = object()\n"
                 "    if f:\n"
                 "        n = 7\n"
                 "        return 1\n"
                 "    else:\n"
                 "        return 0\n");
}

// visit(While) has its OWN join, separate code from visit(For)'s, so the
// `for`-bodied ThePostLoopJoinIncludesThePreLoopEdgeNotJustEndOfBody above
// cannot pin it. Same assertion, same reasoning, a `while` body instead:
// the correct join is `int | str` (pre-loop `int` union end-of-body `str`),
// and a Union operand defers every operator (NotImplementedError), which the
// end-of-body `str` alone would not -- that would be a plain TypeError on
// `str + int`. Verified against mypy 1.18.1: `Unsupported operand types for
// + ("str" and "int")`, left operand `int | str`, on the return line.
TEST(TypeChecker, TheWhilePostLoopJoinIncludesThePreLoopEdgeNotJustEndOfBody) {
    const Checked checked = check_module("class Bag:\n"
                                         "    n: object = object()\n"
                                         "    def m(self, f: bool) -> int:\n"
                                         "        self.n = 7\n"
                                         "        while f:\n"
                                         "            self.n = \"s\"\n"
                                         "        return self.n + 1\n");
    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "NotImplementedError");
    EXPECT_EQ(error.message, "operations on a union-typed value require "
                             "narrowing, which is not supported");
    EXPECT_EQ(error.line, 7);
}

// THE `for`-TARGET SNAPSHOT ORDERING. The zero-iteration edge is the state
// in which the target was never assigned, so the pre-loop snapshot must be
// taken BEFORE the target is bound. Snapshotting after puts the element type
// on both edges, the join collapses to `int`, and this comes out silently
// clean -- accepting a program mypy rejects.
//
// Verified against mypy 1.18.1: `Unsupported operand types for + ("object"
// and "int")` on the `return` line; CPython 3.13.5 runs the file, printing
// `1`, `2`, `3`. Both the code and the line are asserted, since the whole
// point of the ordering is which type reaches that one read.
TEST(TypeChecker, AForTargetsPreLoopStateIsOnTheZeroIterationEdge) {
    const Checked checked = check_module("def m(xs: list[int]) -> int:\n"
                                         "    x: object = \"s\"\n"
                                         "    for x in xs:\n"
                                         "        print(x)\n"
                                         "    return x + 1\n");
    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message,
              "unsupported operand types for + (\"object\" and \"int\")");
    EXPECT_EQ(error.line, 5);
}

// THE CONTROL for the ordering above: a target with no prior binding at all.
// This is the case an "after the bind" snapshot was originally defended
// with, and it is clean either way -- with nothing bound before the loop
// there is no pre-loop entry for the join to widen against. Verified against
// mypy 1.18.1: `Success`; CPython 3.13.5 prints `1`, `2`, `3`.
TEST(TypeChecker, AForTargetWithNoPriorBindingStaysCleanAfterTheLoop) {
    expect_clean("def m(xs: list[int]) -> int:\n"
                 "    for x in xs:\n"
                 "        print(x)\n"
                 "    return x + 1\n");
}

// `break` AND `continue` ARE TERMINATORS TOO, not just `return`. Both are
// parsed, so both reach visit(If)'s join, and an edge kept from a branch
// control never leaves widens the surviving branch's narrowing back to the
// declared type -- a false TypeError on a program both oracles accept.
// Verified against mypy 1.18.1 and CPython 3.14.2 for this exact source with
// `print(m([1, 2], True))` / `print(m([1, 2], False))` appended: mypy
// `Success`, CPython prints `16` then `0`.
TEST(TypeChecker, AContinuingElseBranchContributesNoEdgeToTheJoin) {
    expect_clean("def m(xs: list[int], f: bool) -> int:\n"
                 "    n: object = object()\n"
                 "    total: int = 0\n"
                 "    for _ in xs:\n"
                 "        if f:\n"
                 "            n = 7\n"
                 "        else:\n"
                 "            continue\n"
                 "        total = total + n + 1\n"
                 "    return total\n");
}

// Same shape, `break` in place of `continue`. Separate test because the two
// are separate AST nodes and a fix that handled only one would leave the
// other reporting. Verified against mypy 1.18.1 and CPython 3.14.2 with the
// same two driver lines: mypy `Success`, CPython prints `16` then `0`.
TEST(TypeChecker, ABreakingElseBranchContributesNoEdgeToTheJoin) {
    expect_clean("def m(xs: list[int], f: bool) -> int:\n"
                 "    n: object = object()\n"
                 "    total: int = 0\n"
                 "    for _ in xs:\n"
                 "        if f:\n"
                 "            n = 7\n"
                 "        else:\n"
                 "            break\n"
                 "        total = total + n + 1\n"
                 "    return total\n");
}

// A `while` body rather than a `for` body: the enclosing loop is a different
// visit() with its own walk, so neither test above pins this one. Verified
// against mypy 1.18.1 and CPython 3.14.2 with `print(m(2, True))` /
// `print(m(2, False))` appended: mypy `Success`, CPython prints `16` then `0`.
TEST(TypeChecker, ABreakingElseBranchContributesNoEdgeToTheJoinInAWhileBody) {
    expect_clean("def m(k: int, f: bool) -> int:\n"
                 "    n: object = object()\n"
                 "    total: int = 0\n"
                 "    while k > 0:\n"
                 "        k = k - 1\n"
                 "        if f:\n"
                 "            n = 7\n"
                 "        else:\n"
                 "            break\n"
                 "        total = total + n + 1\n"
                 "    return total\n");
}

// Whatever follows a `break` in the same suite is dead code, so the branch
// still never falls through -- the terminator does not have to be the LAST
// statement. Verified against mypy 1.18.1 and CPython 3.14.2 with the same
// two driver lines as the `for` tests above: mypy `Success`, CPython prints
// `16` then `0`.
TEST(TypeChecker, DeadCodeAfterABreakDoesNotRestoreTheEdge) {
    expect_clean("def m(xs: list[int], f: bool) -> int:\n"
                 "    n: object = object()\n"
                 "    total: int = 0\n"
                 "    for _ in xs:\n"
                 "        if f:\n"
                 "            n = 7\n"
                 "        else:\n"
                 "            break\n"
                 "            total = total + 1\n"
                 "        total = total + n + 1\n"
                 "    return total\n");
}

// The three terminators may be MIXED across a nested `if`'s two arms: an arm
// that breaks and an arm that continues both leave, so the nested `if` leaves
// and the branch containing it contributes no edge. Verified against mypy
// 1.18.1 and CPython 3.14.2 with `print(m([1, 2], True, True))` /
// `print(m([1, 2], False, False))` appended: mypy `Success`, CPython prints
// `16` then `0`.
TEST(TypeChecker, AnIfArmMixingBreakAndContinueLeavesTheBranch) {
    expect_clean("def m(xs: list[int], f: bool, g: bool) -> int:\n"
                 "    n: object = object()\n"
                 "    total: int = 0\n"
                 "    for _ in xs:\n"
                 "        if f:\n"
                 "            n = 7\n"
                 "        else:\n"
                 "            if g:\n"
                 "                break\n"
                 "            else:\n"
                 "                continue\n"
                 "        total = total + n + 1\n"
                 "    return total\n");
}

// One arm returning and the other breaking leaves NO edge to keep, the same
// situation as both arms returning -- keeping both is the fallback, and this
// pins that it neither crashes nor invents a diagnostic.
//
// THE FIXTURE NOW READS A NARROWED PATH AFTER THE MERGE, which the original
// version of this test did not: its `total = total + 1` involved no narrowed
// path at all, so it passed at 2997f6f while the defect it is named for was
// live -- one of four guard tests on this task that were vacuous in exactly
// that way. `n` is declared `object` and narrowed to `int` only in the
// breaking arm, so a merge state that widens it back to `object` makes
// `total + n + 1` a TypeError. Verified against mypy 1.18.1 and CPython
// 3.14.2 with `print(m([1, 2], True))` / `print(m([1, 2], False))` appended:
// mypy `Success: no issues found in 1 source file`, CPython prints `0` twice.
TEST(TypeChecker, ABreakingBranchAndAReturningBranchKeepBothEdgesRatherThanNone) {
    expect_clean("def m(xs: list[int], f: bool) -> int:\n"
                 "    n: object = object()\n"
                 "    total: int = 0\n"
                 "    for _ in xs:\n"
                 "        if f:\n"
                 "            n = 7\n"
                 "            break\n"
                 "        else:\n"
                 "            return 0\n"
                 "        total = total + n + 1\n"
                 "    return total\n");
}

// THE CONTROL that keeps the tests above from being satisfiable by treating
// any TEXTUAL `break` as a terminator: a `break` guarded by a nested `if`
// with no `else` is CONDITIONAL, so that branch can still fall through and
// its edge must still contribute -- widening `n` back to `object` exactly as
// mypy does. Verified against mypy 1.18.1: `Unsupported operand types for +
// ("int" and "object")` on the `total = total + n + 1` line.
TEST(TypeChecker, AConditionalBreakStillContributesItsEdge) {
    const Checked checked = check_module("def m(xs: list[int], f: bool, g: bool) -> int:\n"
                                         "    n: object = object()\n"
                                         "    total: int = 0\n"
                                         "    for _ in xs:\n"
                                         "        if f:\n"
                                         "            n = 7\n"
                                         "        else:\n"
                                         "            if g:\n"
                                         "                break\n"
                                         "        total = total + n + 1\n"
                                         "    return total\n");
    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message,
              "unsupported operand types for + (\"int\" and \"object\")");
    EXPECT_EQ(error.line, 10);
}

// THE SECOND CONTROL, and the distinction contains_reachable_break already
// draws for its own question: a `continue` inside a NESTED loop belongs to
// THAT loop, so it does not leave the branch the nested loop sits in, and
// that branch's edge must still contribute. Verified against mypy 1.18.1:
// `Unsupported operand types for + ("int" and "object")` on the
// `total = total + n + 1` line.
TEST(TypeChecker, ATerminatorInANestedLoopsBodyDoesNotLeaveTheBranch) {
    const Checked checked = check_module("def m(xs: list[int], f: bool) -> int:\n"
                                         "    n: object = object()\n"
                                         "    total: int = 0\n"
                                         "    for _ in xs:\n"
                                         "        if f:\n"
                                         "            n = 7\n"
                                         "        else:\n"
                                         "            for _y in xs:\n"
                                         "                continue\n"
                                         "        total = total + n + 1\n"
                                         "    return total\n");
    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message,
              "unsupported operand types for + (\"int\" and \"object\")");
    EXPECT_EQ(error.line, 10);
}

// The OPPOSITE half of that distinction: a nested loop's ORELSE runs outside
// the nested loop's own break scope, so a `break` written there targets the
// ENCLOSING loop and does leave the branch -- provided the nested loop's body
// cannot break out, which is what makes its `else` always run. Verified
// against mypy 1.18.1: `Success`.
TEST(TypeChecker, ABreakInANestedLoopsOrelseLeavesTheBranch) {
    expect_clean("def m(xs: list[int], f: bool) -> int:\n"
                 "    n: object = object()\n"
                 "    total: int = 0\n"
                 "    for _ in xs:\n"
                 "        if f:\n"
                 "            n = 7\n"
                 "        else:\n"
                 "            for _y in xs:\n"
                 "                pass\n"
                 "            else:\n"
                 "                break\n"
                 "        total = total + n + 1\n"
                 "    return total\n");
}

// THE GUARD ON THE OTHER PREDICATE: always_leaves_branch must stay SEPARATE
// from always_returns, never a widening of it. A `break` is not a `return`,
// so a `while True` whose every path breaks still falls out of the loop and
// still needs a return after it -- widening always_returns to count `break`
// would silence this. Verified against mypy 1.18.1: `Missing return
// statement  [return]`.
TEST(TypeChecker, ABreakingWhileTrueStillNeedsAReturnAfterIt) {
    const Checked checked = check_module("def f(c: bool) -> int:\n"
                                         "    while True:\n"
                                         "        if c:\n"
                                         "            break\n"
                                         "        else:\n"
                                         "            break\n");
    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "missing return statement");
    EXPECT_EQ(error.line, 1);
}

// ---------------------------------------------------------------------------
// UNREACHABLE CODE: TypeError is suppressed, the walk is not.
//
// Every "clean" fixture in this section READS A NARROWED PATH after the merge
// or the terminator, and every one of them was proven non-vacuous by
// neutering check_suite (see the round-3 report for the exact failure sets).
// The rule that earns that paragraph: four earlier guard tests on this defect
// passed while the shape they were named for was broken, because their
// fixtures had nothing after the merge point to read.
// ---------------------------------------------------------------------------

// THE REPORTED DEFECT. Both arms of the nested `if` leave (one breaks, one
// returns), so `total = total + n + 1` is dead code -- and at 2997f6f it drew
// `TypeError: unsupported operand types for + ("int" and "object")` at 10:17,
// because the empty-edges fallback restored `n` to its declared `object` and
// the checker type-checked the dead statement anyway. Measured 2026-09-10:
//   $ mypy --strict --no-color-output --no-error-summary f1.py   -> exit 0
//   $ python f1.py                                               -> 0 / 0
// Both oracles accept it, so the diagnostic was a false positive.
TEST(TypeChecker, AStatementAfterABranchThatAlwaysLeavesDrawsNoTypeError) {
    expect_clean("def m(xs: list[int], f: bool) -> int:\n"
                 "    n: object = object()\n"
                 "    total: int = 0\n"
                 "    for _ in xs:\n"
                 "        if f:\n"
                 "            n = 7\n"
                 "            break\n"
                 "        else:\n"
                 "            return 0\n"
                 "        total = total + n + 1\n"
                 "    return total\n");
}

// The same with the arms mirrored -- measured identically: mypy exit 0,
// CPython prints `0` twice.
TEST(TypeChecker, TheMirroredUnreachableMergeIsAlsoClean) {
    expect_clean("def m(xs: list[int], f: bool) -> int:\n"
                 "    n: object = object()\n"
                 "    total: int = 0\n"
                 "    for _ in xs:\n"
                 "        if f:\n"
                 "            return 0\n"
                 "        else:\n"
                 "            n = 7\n"
                 "            break\n"
                 "        total = total + n + 1\n"
                 "    return total\n");
}

// BOTH ARMS THE SAME TERMINATOR. `n = 7` is in neither arm here, so the dead
// statement reads `n` at its declared `object` no matter which edge the join
// keeps -- which is exactly why no choice of merge state could have fixed
// this and the suppression had to be structural. Verified against mypy 1.18.1
// and CPython 3.14.2 with `print(m([1], True))` / `print(m([], False))`:
// mypy `Success: no issues found in 1 source file`, CPython prints `0` twice.
TEST(TypeChecker, BothArmsBreakingLeavesTheRemainderOfTheLoopBodyUnchecked) {
    expect_clean("def m(xs: list[int], f: bool) -> int:\n"
                 "    n: object = object()\n"
                 "    total: int = 0\n"
                 "    for _ in xs:\n"
                 "        if f:\n"
                 "            break\n"
                 "        else:\n"
                 "            break\n"
                 "        total = total + n + 1\n"
                 "    return total\n");
}

// Both arms RETURNING, at function-body level rather than in a loop. Verified
// against mypy 1.18.1: `Success`; with `print(f(True))` / `print(f(False))`
// appended CPython prints `0` then `1`.
TEST(TypeChecker, BothArmsReturningLeavesTheRemainderOfTheBodyUnchecked) {
    expect_clean("def f(c: bool) -> int:\n"
                 "    total: int = 0\n"
                 "    if c:\n"
                 "        return total\n"
                 "    else:\n"
                 "        return total + 1\n"
                 "    total = total + \"s\"\n");
}

// A BARE `break`, with no nested `if` in the way. Verified against mypy
// 1.18.1: `Success`; CPython prints `0`.
TEST(TypeChecker, AStatementAfterABareBreakIsNotTypeChecked) {
    expect_clean("def f() -> int:\n"
                 "    total: int = 0\n"
                 "    for i in range(3):\n"
                 "        total = total + i\n"
                 "        break\n"
                 "        total = total + \"s\"\n"
                 "    return total\n");
}

// A BARE `continue`. Verified against mypy 1.18.1: `Success`; CPython prints
// `3`.
TEST(TypeChecker, AStatementAfterABareContinueIsNotTypeChecked) {
    expect_clean("def f() -> int:\n"
                 "    total: int = 0\n"
                 "    for i in range(3):\n"
                 "        total = total + i\n"
                 "        continue\n"
                 "        total = total + \"s\"\n"
                 "    return total\n");
}

// A `for ... else: return` whose body has NO break: the `else` always runs,
// so what follows it is dead. Verified against mypy 1.18.1: `Success`.
// Written `-> None` deliberately -- the `-> int` form draws a SEPARATE,
// PRE-EXISTING false `missing return statement` from always_returns, which
// does not carry this clause; see that predicate's own comment.
TEST(TypeChecker, AStatementAfterAForElseReturnIsNotTypeChecked) {
    expect_clean("def f() -> None:\n"
                 "    total: int = 0\n"
                 "    for i in range(3):\n"
                 "        total = total + i\n"
                 "    else:\n"
                 "        return\n"
                 "    total = total + \"s\"\n");
}

// The `while cond ... else: return` form of the same. Verified against mypy
// 1.18.1: `Success`. `-> None` for the same reason as above.
TEST(TypeChecker, AStatementAfterAWhileElseReturnIsNotTypeChecked) {
    expect_clean("def f() -> None:\n"
                 "    total: int = 0\n"
                 "    while total < 3:\n"
                 "        total = total + 1\n"
                 "    else:\n"
                 "        return\n"
                 "    total = total + \"s\"\n");
}

// A `while True:` with no break never falls out of the loop at all. Verified
// against mypy 1.18.1: `Success`; CPython prints `1`.
TEST(TypeChecker, AStatementAfterAWhileTrueThatReturnsIsNotTypeChecked) {
    expect_clean("def f() -> int:\n"
                 "    total: int = 0\n"
                 "    while True:\n"
                 "        total = total + 1\n"
                 "        return total\n"
                 "    total = total + \"s\"\n");
}

// ---------------------------------------------------------------------------
// The discriminators between "suppress TypeError" and "stop walking". Every
// one of them PASSES here and FAILS under a check_suite that returns early at
// the terminator, which is exactly why the walk continues.
// ---------------------------------------------------------------------------

// A NAME BOUND ONLY IN UNREACHABLE CODE still binds, and a statically
// reachable read of it resolves. Measured 2026-09-10 on exactly this program
// plus a `print("module ran")` line:
//   $ mypy --strict --no-color-output --no-error-summary c10.py
//   Success: no issues found in 1 source file
//   $ python c10.py
//   module ran                     (exit 0)
// Both oracles accept it, so a `NameError` here would be a false positive --
// which is precisely what dropping the walk produces. The ANNOTATION on `x`
// is load-bearing: mypy reports `Cannot determine type of "x"  [has-type]`
// for the unannotated `x = 1`, so only the annotated form is clean.
TEST(TypeChecker, ANameBoundOnlyInUnreachableCodeStillResolves) {
    expect_clean("def f(c: bool) -> None:\n"
                 "    if c:\n"
                 "        return\n"
                 "        x: int = 1\n"
                 "    print(x)\n");
}

// NameError IS STILL REPORTED in unreachable code, because mypy reports it
// there. Measured 2026-09-10:
//   $ mypy --strict --no-color-output --no-error-summary e01.py
//   e01.py:3: error: Name "nope_not_defined" is not defined  [name-defined]
// mypy's semantic analyzer runs everywhere; only its type checker stops. A
// checker that skipped the subtree would go silent and miss an error mypy
// reports.
TEST(TypeChecker, AnUndefinedNameInUnreachableCodeStillReportsNameError) {
    const Checked checked = check_module("def f() -> int:\n"
                                         "    return 0\n"
                                         "    print(nope_not_defined)\n");
    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "NameError");
    EXPECT_EQ(error.message, "name 'nope_not_defined' is not defined");
    EXPECT_EQ(error.line, 3);
}

// NotImplementedError SURVIVES in unreachable code, deliberately: it is this
// compiler's own capability claim ("cannot model this construct"), not a mypy
// type judgement, the code still has to be emitted as C++, and it is not
// silent acceptance -- so keeping it cannot violate the union rule. mypy is
// silent on this file (measured: `Success`), and that is fine, because
// NotImplementedError is not a claim about mypy.
TEST(TypeChecker, NotImplementedErrorSurvivesInUnreachableCode) {
    const Checked checked = check_module("def f() -> int:\n"
                                         "    return 0\n"
                                         "    s = \"a\"\n"
                                         "    s.upper()\n");
    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "NotImplementedError");
    EXPECT_EQ(error.message, "methods on builtin types are not supported");
    EXPECT_EQ(error.line, 4);
}

// OverflowError likewise survives, for the same reason: a literal that does
// not fit 64 bits is still a literal this compiler cannot emit, wherever it
// stands. mypy is silent on this file (measured: `Success`).
TEST(TypeChecker, OverflowErrorSurvivesInUnreachableCode) {
    const Checked checked = check_module("def f() -> int:\n"
                                         "    return 0\n"
                                         "    x = 99999999999999999999999\n");
    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "OverflowError");
    EXPECT_EQ(error.line, 3);
}

// NARROWING MUST NOT ESCAPE AN UNREACHABLE STATEMENT. `n = "s"` is dead code
// so its own TypeError is suppressed -- but if it still recorded a narrowing,
// the loop's end-of-body snapshot would carry `n` as `str`, the loop join
// would yield `int | str`, and the REACHABLE `k: int = n` would draw a
// diagnostic from OUTSIDE the suppressed region. That is exactly what 2997f6f
// did: `10:5: TypeError: incompatible types in assignment (expression has
// type "int | str", variable has type "int")`. Measured 2026-09-10:
//   $ mypy --strict --no-color-output --no-error-summary leak.py   -> exit 0
//   $ python leak.py                                               -> 7 / 7
// So a false positive on reachable code, and a union-rule violation. The fix
// is check_suite restoring the narrowing state as of the terminator.
TEST(TypeChecker, NarrowingDoesNotEscapeAnUnreachableStatement) {
    expect_clean("def m(f: bool) -> int:\n"
                 "    n: object = object()\n"
                 "    n = 7\n"
                 "    while f:\n"
                 "        if f:\n"
                 "            break\n"
                 "        else:\n"
                 "            break\n"
                 "        n = \"s\"\n"
                 "    k: int = n\n"
                 "    return k\n");
}

// The NotImplementedError-flavoured form of the same leak: a stored
// `int | str` defers every operator applied to it, so at 2997f6f the
// reachable `n + 1` drew `10:12: NotImplementedError: operations on a
// union-typed value require narrowing, which is not supported`. Measured
// 2026-09-10: mypy exit 0, CPython prints `8` twice.
TEST(TypeChecker, AnEscapedNarrowingDoesNotDeferAReachableOperator) {
    expect_clean("def m(f: bool) -> int:\n"
                 "    n: object = object()\n"
                 "    n = 7\n"
                 "    while f:\n"
                 "        if f:\n"
                 "            break\n"
                 "        else:\n"
                 "            break\n"
                 "        n = \"s\"\n"
                 "    return n + 1\n");
}

// ---------------------------------------------------------------------------
// Controls: the suppression region must END where it should.
// ---------------------------------------------------------------------------

// THE GUARD IS POPPED. An `if` with no `else` can fall through, so the
// statement after it is REACHABLE and its type error must still report --
// even though the `if`'s own body contains an unreachable region. Verified
// against mypy 1.18.1: `Unsupported operand types for + ("int" and "str")` on
// line 6 and nothing on line 5.
TEST(TypeChecker, AReachableTypeErrorAfterAnUnreachableRegionStillReports) {
    const Checked checked = check_module("def f(c: bool) -> int:\n"
                                         "    total: int = 0\n"
                                         "    if c:\n"
                                         "        return 0\n"
                                         "        total = total + \"s\"\n"
                                         "    total = total + \"s\"\n"
                                         "    return total\n");
    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "unsupported operand types for + (\"int\" and \"str\")");
    EXPECT_EQ(error.line, 6);
}

// A SIBLING ARM IS NOT SUPPRESSED. The `if` body goes unreachable after its
// `return`, but the `else` arm is a DIFFERENT suite and is fully reachable.
// Verified against mypy 1.18.1: `Unsupported operand types for + ("int" and
// "str")` on line 7 only.
TEST(TypeChecker, TheElseArmIsNotSuppressedByItsSiblingsUnreachableRegion) {
    const Checked checked = check_module("def f(c: bool) -> int:\n"
                                         "    total: int = 0\n"
                                         "    if c:\n"
                                         "        return 0\n"
                                         "        total = total + \"s\"\n"
                                         "    else:\n"
                                         "        total = total + \"s\"\n"
                                         "    return total\n");
    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "unsupported operand types for + (\"int\" and \"str\")");
    EXPECT_EQ(error.line, 7);
}

// REGIONS NEST, so the suppression is a DEPTH COUNTER and not a bool: an
// unreachable `if` inside an unreachable suite pushes a second region, and
// popping the inner one must not un-suppress the outer. Lines 7 and 8 sit
// inside two and one nested regions respectively; line 9 is reachable.
// Verified against mypy 1.18.1: `Unsupported operand types for + ("int" and
// "str")` on line 9 alone.
TEST(TypeChecker, NestedUnreachableRegionsPopBackToReachable) {
    const Checked checked = check_module("def f(c: bool, d: bool) -> int:\n"
                                         "    total: int = 0\n"
                                         "    if c:\n"
                                         "        return 0\n"
                                         "        if d:\n"
                                         "            return 1\n"
                                         "            total = total + \"s\"\n"
                                         "        total = total + \"s\"\n"
                                         "    total = total + \"s\"\n"
                                         "    return total\n");
    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "unsupported operand types for + (\"int\" and \"str\")");
    EXPECT_EQ(error.line, 9);
}

// ---------------------------------------------------------------------------
// Controls, one per skipping rule, where control GENUINELY falls through.
// A too-eager predicate would silence REACHABLE code -- a missed error the
// suite would otherwise never notice. All five were measured as REPORTING by
// mypy 1.18.1, and all five raise under CPython on the falling-through path.
// ---------------------------------------------------------------------------

// An `if` with NO `else` always has a fall-through edge.
TEST(TypeChecker, AnIfWithNoElseThatReturnsLeavesTheRestReachable) {
    const Checked checked = check_module("def f(c: bool) -> int:\n"
                                         "    total: int = 0\n"
                                         "    if c:\n"
                                         "        return total\n"
                                         "    total = total + \"s\"\n"
                                         "    return total\n");
    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "unsupported operand types for + (\"int\" and \"str\")");
    EXPECT_EQ(error.line, 5);
}

// A REACHABLE `break` in the body means the `else` can be SKIPPED, so
// `else: return` no longer terminates the enclosing suite. This is the subtle
// one: a checker that read `else: return` as an unconditional terminator
// without looking for a break would silently accept a program both oracles
// reject.
TEST(TypeChecker, AReachableBreakWithAnElseReturnLeavesTheRestReachable) {
    const Checked checked = check_module("def f(c: bool) -> int:\n"
                                         "    total: int = 0\n"
                                         "    for i in range(3):\n"
                                         "        if c:\n"
                                         "            break\n"
                                         "    else:\n"
                                         "        return total\n"
                                         "    total = total + \"s\"\n"
                                         "    return total\n");
    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "unsupported operand types for + (\"int\" and \"str\")");
    EXPECT_EQ(error.line, 8);
}

// A `while True:` that DOES have a reachable break falls out of the loop.
TEST(TypeChecker, AWhileTrueWithAReachableBreakLeavesTheRestReachable) {
    const Checked checked = check_module("def f(c: bool) -> int:\n"
                                         "    total: int = 0\n"
                                         "    while True:\n"
                                         "        if c:\n"
                                         "            break\n"
                                         "    total = total + \"s\"\n"
                                         "    return total\n");
    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "unsupported operand types for + (\"int\" and \"str\")");
    EXPECT_EQ(error.line, 6);
}

// Only ONE arm returning leaves the other arm's fall-through edge.
TEST(TypeChecker, AnIfWhereOnlyOneArmReturnsLeavesTheRestReachable) {
    const Checked checked = check_module("def f(c: bool) -> int:\n"
                                         "    total: int = 0\n"
                                         "    if c:\n"
                                         "        return total\n"
                                         "    else:\n"
                                         "        total = total + 1\n"
                                         "    total = total + \"s\"\n"
                                         "    return total\n");
    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "unsupported operand types for + (\"int\" and \"str\")");
    EXPECT_EQ(error.line, 7);
}

// A `break` in a NESTED loop belongs to that loop and does not terminate the
// OUTER body.
TEST(TypeChecker, ABreakInANestedLoopLeavesTheOuterBodyReachable) {
    const Checked checked = check_module("def f() -> int:\n"
                                         "    total: int = 0\n"
                                         "    for i in range(2):\n"
                                         "        for j in range(2):\n"
                                         "            break\n"
                                         "        total = total + \"s\"\n"
                                         "    return total\n");
    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "unsupported operand types for + (\"int\" and \"str\")");
    EXPECT_EQ(error.line, 6);
}

// ---------------------------------------------------------------------------
// A TERMINATOR CPYTHON REFUSES TO COMPILE DOES NOT START A REGION.
//
// Every row below is a program BOTH ORACLES REJECT, where the type error on
// the following line is the only diagnostic this checker has. Suppressing it
// would turn a rejection into a silent acceptance -- a union-rule violation.
// mypy's own silence about that following line is NOT reachability pruning:
// it is a BLOCKING error (exit 2 for break/continue, no bracketed code), so
// mypy never reached type checking at all. Measured 2026-09-10 and quoted in
// statement_always_leaves' own comment.
// ---------------------------------------------------------------------------

TEST(TypeChecker, AModuleLevelBreakDoesNotStartAnUnreachableRegion) {
    const Checked checked = check_module("x: int = 0\n"
                                         "break\n"
                                         "y: int = \"s\"\n");
    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "incompatible types in assignment (expression has type \"str\", "
                             "variable has type \"int\")");
    EXPECT_EQ(error.line, 3);
}

TEST(TypeChecker, AModuleLevelContinueDoesNotStartAnUnreachableRegion) {
    const Checked checked = check_module("x: int = 0\n"
                                         "continue\n"
                                         "y: int = \"s\"\n");
    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.line, 3);
}

TEST(TypeChecker, AModuleLevelReturnDoesNotStartAnUnreachableRegion) {
    const Checked checked = check_module("x: int = 0\n"
                                         "return\n"
                                         "y: int = \"s\"\n");
    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.line, 3);
}

// A CLASS BODY resets both context flags, even inside a function or a loop.
// Measured: `for i in range(2): / class C: / a: int = 0 / break` is
// `"break" outside loop` under mypy (exit 2) and a CPython SyntaxError, and
// `def f(): / class C: / return` is `"return" outside function  [misc]`.
TEST(TypeChecker, AClassBodyReturnDoesNotStartAnUnreachableRegion) {
    const Checked checked = check_module("class C:\n"
                                         "    a: int = 0\n"
                                         "    return\n"
                                         "    b: int = \"s\"\n");
    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.line, 4);
}

// A `break` inside a loop's own BODY is legal, and the region it opens ends
// with that body -- the rest of the MODULE stays reachable. Verified against
// mypy 1.18.1: `Incompatible types in assignment (expression has type "str",
// variable has type "int")  [assignment]` on line 4.
TEST(TypeChecker, ALoopBodyBreakLeavesTheRestOfTheModuleReachable) {
    const Checked checked = check_module("x: int = 0\n"
                                         "for i in range(3):\n"
                                         "    break\n"
                                         "y: int = \"s\"\n");
    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.line, 4);
}

// THE ONE LEGAL module-scope shape, and the direction the context gate must
// NOT block: a `while True:` with no break involves no terminator at all, so
// what follows it really is unreachable. Measured 2026-09-10:
//   $ mypy --strict --no-color-output --no-error-summary d04.py
//   (no output, exit 0)
// 2997f6f reported a false `TypeError: incompatible types in assignment` on
// the last line here; this is a FIX, not a preserved behaviour.
TEST(TypeChecker, AModuleLevelWhileTrueWithNoBreakDoesStartAnUnreachableRegion) {
    expect_clean("x: int = 0\n"
                 "while True:\n"
                 "    x = x + 1\n"
                 "y: int = \"s\"\n");
}

// ---------------------------------------------------------------------------
// UNREACHABLE CODE SUPPRESSES MYPY'S TYPE CHECKER, NOT ITS SEMANTIC ANALYZER.
//
// Every test below sits inside an unreachable region and MUST still report,
// because mypy reports its analogue there. They are the cases lost by keying
// suppression on the code string "TypeError", which cythonpp spells on
// both sides of that line. Each was measured against mypy 1.18.1 and CPython
// 3.14.2 on 2026-09-10; the verbatim mypy line is quoted per test.
// ---------------------------------------------------------------------------

// 1a. An annotated redefinition after a `return`.
//   $ mypy --strict --no-color-output --no-error-summary q.py
//   q.py:4: error: Name "y" already defined on line 3  [no-redef]   (exit 1)
//   $ python q.py    -> no output (exit 0)
TEST(TypeChecker, AnAnnotatedRedefinitionAfterAReturnStillReports) {
    const Checked checked = check_module("def f() -> None:\n"
                                         "    return\n"
                                         "    y: int = 1\n"
                                         "    y: str = \"s\"\n");
    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "name \"y\" already defined on line 3");
    EXPECT_EQ(error.line, 4);
}

// 1b. A nested `def` redefined after a `return`.
//   $ mypy --strict ... r.py
//   r.py:5: error: Name "g" already defined on line 3  [no-redef]   (exit 1)
//   $ python r.py    -> no output (exit 0)
TEST(TypeChecker, ANestedDefRedefinedInUnreachableCodeStillReports) {
    const Checked checked = check_module("def f() -> None:\n"
                                         "    return\n"
                                         "    def g() -> int:\n"
                                         "        return 0\n"
                                         "    def g() -> int:\n"
                                         "        return 1\n");
    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "name \"g\" already defined on line 3");
    EXPECT_EQ(error.line, 5);
}

// 1c. After a bare `break`, so the region opens on a loop terminator rather
// than a `return`.
//   $ mypy --strict ... s2.py
//   s2.py:6: error: Name "z" already defined on line 5  [no-redef]  (exit 1)
//   $ python s2.py (with print(f()) appended) -> 0 (exit 0)
TEST(TypeChecker, ARedefinitionAfterABareBreakStillReports) {
    const Checked checked = check_module("def f() -> int:\n"
                                         "    total: int = 0\n"
                                         "    for i in range(3):\n"
                                         "        break\n"
                                         "        z: int = 1\n"
                                         "        z: str = \"s\"\n"
                                         "    return total\n");
    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "name \"z\" already defined on line 5");
    EXPECT_EQ(error.line, 6);
}

// 1d. After an `if`/`else` whose arms both return -- the shape that reaches
// the empty-edges fallback in visit(If) as well as the region.
//   $ mypy --strict ... s3.py
//   s3.py:7: error: Name "w" already defined on line 6  [no-redef]  (exit 1)
//   $ python s3.py (with print(f(True)) appended) -> 0 (exit 0)
TEST(TypeChecker, ARedefinitionAfterABothArmsReturningIfStillReports) {
    const Checked checked = check_module("def f(c: bool) -> int:\n"
                                         "    if c:\n"
                                         "        return 0\n"
                                         "    else:\n"
                                         "        return 1\n"
                                         "    w: int = 1\n"
                                         "    w: str = \"s\"\n");
    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "name \"w\" already defined on line 6");
    EXPECT_EQ(error.line, 7);
}

// 1e. THE WORST ONE: a duplicate parameter name, which BOTH oracles reject.
// mypy's error is BLOCKING (exit 2, no bracketed code) and CPython refuses to
// compile the file at all, so it never runs at any reachability -- there is no
// reading of reachability under which silence here is defensible.
//   $ mypy --strict ... v3.py
//   v3.py:3: error: Duplicate argument "x" in function definition   (exit 2)
//   $ python v3.py
//   SyntaxError: duplicate argument 'x' in function definition      (exit 1)
TEST(TypeChecker, ADuplicateArgumentInUnreachableCodeStillReports) {
    const Checked checked = check_module("def f() -> None:\n"
                                         "    return\n"
                                         "    def g(x: int, x: str) -> None:\n"
                                         "        pass\n");
    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "duplicate argument \"x\" in function definition");
    EXPECT_EQ(error.line, 3);
}

// A class-body redefinition inside an unreachable region: the same
// bind_annotation site as 1a, reached through a ScopeKind::Class push, so the
// classification cannot be scope-dependent.
//   $ mypy --strict ... r1.py
//   r1.py:5: error: Name "y" already defined on line 4  [no-redef]  (exit 1)
//   $ python r1.py   -> no output (exit 0)
TEST(TypeChecker, AClassBodyRedefinitionInUnreachableCodeStillReports) {
    const Checked checked = check_module("def f() -> None:\n"
                                         "    return\n"
                                         "    class C:\n"
                                         "        y: int = 1\n"
                                         "        y: str = \"s\"\n");
    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "name \"y\" already defined on line 4");
    EXPECT_EQ(error.line, 5);
}

// WHETHER AN ANNOTATION IS A WELL-FORMED TYPE is also semantic-analyzer
// output -- mypy's [valid-type] and [type-arg] -- and it is reported in
// unreachable code just like [no-redef]. This class was NOT among the five
// shapes the round-3 review found; it turned up in the per-site audit of what
// each "TypeError" site actually asserts. Four sub-classes, one test each,
// all four measured reported by mypy in unreachable position.
//
//   $ mypy --strict ... an1.py
//   an1.py:3: error: Invalid type: try using Literal[5] instead?  [valid-type]
TEST(TypeChecker, AnInvalidAnnotationInUnreachableCodeStillReports) {
    const Checked checked = check_module("def f() -> None:\n"
                                         "    return\n"
                                         "    x: 5 = 1\n");
    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "not a valid type annotation");
    EXPECT_EQ(error.line, 3);
}

//   $ mypy --strict ... an2.py
//   an2.py:3: error: "list" expects 1 type argument, but 2 given  [type-arg]
TEST(TypeChecker, AWrongArityAnnotationInUnreachableCodeStillReports) {
    const Checked checked = check_module("def f() -> None:\n"
                                         "    return\n"
                                         "    x: list[int, str] = []\n");
    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "\"list\" expects 1 type argument, but 2 given");
    EXPECT_EQ(error.line, 3);
}

//   $ mypy --strict ... an3.py
//   an3.py:3: error: "int" expects no type arguments, but 1 given  [type-arg]
TEST(TypeChecker, ASubscriptedNonGenericAnnotationInUnreachableCodeStillReports) {
    const Checked checked = check_module("def f() -> None:\n"
                                         "    return\n"
                                         "    x: int[str] = 1\n");
    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "'int' is not subscriptable");
    EXPECT_EQ(error.line, 3);
}

//   $ mypy --strict ... an5.py
//   an5.py:3: error: Missing type parameters for generic type "list"  [type-arg]
TEST(TypeChecker, ABareGenericAnnotationInUnreachableCodeStillReports) {
    const Checked checked = check_module("def f() -> None:\n"
                                         "    return\n"
                                         "    x: list = []\n");
    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "missing type parameters for generic type \"list\"");
    EXPECT_EQ(error.line, 3);
}


// ---------------------------------------------------------------------------
// Four conditional-`def` shapes and their REACHABLE twins. Every one of the
// eight is a program `mypy --strict` and CPython BOTH accept, and every one
// was once rejected with a `TypeError`. The reachable form is the original
// defect; the unreachable form is the extra reach that un-suppressing
// semantic-analyzer diagnostics in unreachable code widened it to. All
// measurements 2026-09-11, mypy 1.18.1 (compiled: yes) / Python 3.14.2.
// ---------------------------------------------------------------------------

// Critical rows 1-3, REACHABLE.
//   $ cat r_type.py            $ cat r_slice.py             $ cat r_memv.py
//   x: type[int] = int         def f() -> None:             def f() -> None:
//   print(x)                       y: slice[int]                y: memoryview[int]
//                              print("ran")                 print("ran")
//   $ mypy --strict --no-color-output --no-error-summary <each> -> exit 0, no output
//   $ python <each>  ->  `<class 'int'>` / `ran` / `ran`, exit 0 each
//   before: 1:4 / 2:8 / 2:8  error: TypeError: 'X' is not subscriptable
// NotImplementedError is the sanctioned "cannot model this construct" answer
// and is not silent acceptance, so it is the right answer for a generic this
// model has no representation for -- what it must not be is a TypeError.
TEST(TypeChecker, ASubscriptedGenericBuiltinIsUnsupportedNotATypeError) {
    for (const std::string& name : {"type", "slice", "memoryview", "ExceptionGroup",
                                    "BaseExceptionGroup"}) {
        const Checked checked = check_module("x: " + name + "[int]\n");
        const diagnostics::Diagnostic error = only_error(checked);
        EXPECT_EQ(error.code, "NotImplementedError") << name;
        EXPECT_EQ(error.message, "generic builtin type '" + name + "' is not supported") << name;
        EXPECT_EQ(error.line, 1) << name;
    }
}

// The same three shapes, UNREACHABLE -- the position un-suppressing
// semantic-analyzer diagnostics exposed.
//   $ cat u_type.py
//   def f() -> None:
//       return
//       x: type[int] = int
//   print("ran")
//   $ mypy --strict ... u_type.py   -> exit 0, no output
//   $ python u_type.py              -> ran   (exit 0)
//   @613a460: silent   @fd616ac: 3:8: error: TypeError: 'type' is not subscriptable
// Not re-masked by restoring suppression: the KIND changed, so the judgement
// is right in both positions now.
TEST(TypeChecker, ASubscriptedGenericBuiltinInUnreachableCodeIsUnsupportedNotATypeError) {
    for (const std::string& name : {"type", "slice", "memoryview"}) {
        const Checked checked = check_module("def f() -> None:\n"
                                             "    return\n"
                                             "    x: " + name + "[int]\n");
        const diagnostics::Diagnostic error = only_error(checked);
        EXPECT_EQ(error.code, "NotImplementedError") << name;
        EXPECT_EQ(error.message, "generic builtin type '" + name + "' is not supported") << name;
        EXPECT_EQ(error.line, 3) << name;
    }
}

// Critical row 4, REACHABLE, plus the seven other conditional-def shapes the
// same gate rejected. mypy's conditional-function-definition allowance is NOT
// scope-limited, and cythonpp's function-scope site had no conditionality
// test at all. Every fixture below: `mypy --strict` exit 0 with no output,
// CPython exit 0. Verbatim before-state, function-scope site:
//   m04 if/else both arms        6:9: TypeError: name "g" already defined on line 3
//   m10 flat then `if`           5:9: ... on line 2
//   m14 two defs in a `while`    5:9: ... on line 3
//   m15 `if c` and `if not c`    6:9: ... on line 3
//   m19 two defs in ONE `if`     5:9: ... on line 3
//   m20 flat then `for` body     5:9: ... on line 2
//   m23 flat then a NESTED `if`  6:13: ... on line 2
//   m32 three arms of if/elif/else   TWO diagnostics, 6:9 and 9:9
TEST(TypeChecker, AConditionalNestedDefIsNotARedefinition) {
    // m04: both arms of an if/else.
    expect_clean("def f(c: bool) -> None:\n"
                 "    if c:\n"
                 "        def g() -> int:\n"
                 "            return 0\n"
                 "    else:\n"
                 "        def g() -> int:\n"
                 "            return 1\n"
                 "    print(g())\n");
    // m10: a FLAT def first, then a conditional one.
    expect_clean("def f(c: bool) -> None:\n"
                 "    def g() -> int:\n"
                 "        return 0\n"
                 "    if c:\n"
                 "        def g() -> int:\n"
                 "            return 1\n"
                 "    print(g())\n");
    // m14: two defs in a `while` body -- both conditional, no `if` involved.
    expect_clean("def f(c: bool) -> None:\n"
                 "    while c:\n"
                 "        def g() -> int:\n"
                 "            return 0\n"
                 "        def g() -> int:\n"
                 "            return 1\n"
                 "        print(g())\n");
    // m15: two separate `if`s rather than one if/else.
    expect_clean("def f(c: bool) -> None:\n"
                 "    if c:\n"
                 "        def g() -> int:\n"
                 "            return 0\n"
                 "    if not c:\n"
                 "        def g() -> int:\n"
                 "            return 1\n"
                 "    print(g())\n");
    // m19: two defs inside the SAME `if` body.
    expect_clean("def f(c: bool) -> None:\n"
                 "    if c:\n"
                 "        def g() -> int:\n"
                 "            return 0\n"
                 "        def g() -> int:\n"
                 "            return 1\n"
                 "        print(g())\n");
    // m20: a `for` body, so the conditional block is a loop with a target.
    expect_clean("def f(xs: list[int]) -> None:\n"
                 "    def g() -> int:\n"
                 "        return 0\n"
                 "    for i in xs:\n"
                 "        def g() -> int:\n"
                 "            return 1\n"
                 "    print(g())\n");
    // m23: nested two blocks deep, so "conditional" is not depth-one only.
    expect_clean("def f(c: bool) -> None:\n"
                 "    def g() -> int:\n"
                 "        return 0\n"
                 "    if c:\n"
                 "        if c:\n"
                 "            def g() -> int:\n"
                 "                return 1\n"
                 "    print(g())\n");
    // m32: three arms, so the allowance is not "at most two definitions".
    expect_clean("def f(c: bool, d: bool) -> None:\n"
                 "    if c:\n"
                 "        def g() -> int:\n"
                 "            return 0\n"
                 "    elif d:\n"
                 "        def g() -> int:\n"
                 "            return 1\n"
                 "    else:\n"
                 "        def g() -> int:\n"
                 "            return 2\n"
                 "    print(g())\n");
}

// m31, an eighth shape the review's table does not have, and the one that
// shows the test is SIGNATURE EQUALITY rather than "was the earlier binding a
// def": the first binding comes from a plain assignment of a function VALUE,
// and mypy is still clean.
//   $ mypy --strict ... m31_fn_alias_then_cond_def.py   -> exit 0, no output
//   before: 6:9: error: TypeError: name "g" already defined on line 4
TEST(TypeChecker, AConditionalNestedDefOverAnAliasOfTheSameSignatureIsClean) {
    expect_clean("def h() -> int:\n"
                 "    return 2\n"
                 "def f(c: bool) -> None:\n"
                 "    g = h\n"
                 "    if c:\n"
                 "        def g() -> int:\n"
                 "            return 0\n"
                 "    print(g())\n");
}

// Critical row 4, UNREACHABLE -- the review's own `rd1.py`, verbatim:
//   def f(c: bool) -> None:
//       return
//       if c:
//           def g() -> int:
//               return 0
//       else:
//           def g() -> int:
//               return 1
//   print("ran")
//   $ mypy --strict ... rd1.py  -> exit 0, no output
//   $ python rd1.py             -> ran   (exit 0)
//   @613a460: silent   @fd616ac: 7:9: TypeError: name "g" already defined on line 4
TEST(TypeChecker, AConditionalNestedDefInUnreachableCodeIsNotARedefinition) {
    expect_clean("def f(c: bool) -> None:\n"
                 "    return\n"
                 "    if c:\n"
                 "        def g() -> int:\n"
                 "            return 0\n"
                 "    else:\n"
                 "        def g() -> int:\n"
                 "            return 1\n");
}

// ---------------------------------------------------------------------------
// CONTROLS for the four shapes above: collision classes that must KEEP
// reporting. Each is a
// program mypy rejects, so silence here would be a false negative -- the
// failure mode a relaxed DEFAULT (rather than a measured rule) would have
// shipped.
// ---------------------------------------------------------------------------

// m03: two FLAT nested defs. The allowance is about the LATER definition, and
// this one is not conditional.
//   $ mypy --strict ... m03_fn_flat.py
//   m03_fn_flat.py:4: error: Name "g" already defined on line 2  [no-redef]
TEST(TypeChecker, TwoFlatNestedDefsStillCollide) {
    const Checked checked = check_module("def f() -> None:\n"
                                         "    def g() -> int:\n"
                                         "        return 0\n"
                                         "    def g() -> int:\n"
                                         "        return 1\n"
                                         "    print(g())\n");
    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "name \"g\" already defined on line 2");
    EXPECT_EQ(error.line, 4);
}

// m21: conditional, but the two signatures DISAGREE. mypy rejects with its
// own wording, so both oracles reject and silence here would be a real miss.
//   $ mypy --strict ... m21_fn_cond_diff_sig.py
//   m21...py:6: error: All conditional function variants must have identical
//                      signatures  [misc]
// cythonpp keeps the `already defined` wording: a wording divergence, not a
// compliance one.
TEST(TypeChecker, AConditionalNestedDefWithADifferentSignatureStillCollides) {
    const Checked checked = check_module("def f(c: bool) -> None:\n"
                                         "    if c:\n"
                                         "        def g() -> int:\n"
                                         "            return 0\n"
                                         "    else:\n"
                                         "        def g(a: int) -> str:\n"
                                         "            return \"s\"\n"
                                         "    print(g)\n");
    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "name \"g\" already defined on line 3");
    EXPECT_EQ(error.line, 6);
}

// m29: the signatures differ ONLY in a default, which
// has_identical_signature compares through defaulted_params -- the one thing
// `is_equivalent` would have thrown away, since its Callable arm deliberately
// ignores that field. mypy rejects this too.
//   $ mypy --strict ... m29_fn_cond_def_same_sig_defaults.py
//   m29...py:6: error: All conditional function variants must have identical
//                      signatures  [misc]
//   m29...py:6: note:     def g(a: int = ...) -> int  /  def g(a: int) -> int
TEST(TypeChecker, AConditionalNestedDefDifferingOnlyInADefaultStillCollides) {
    const Checked checked = check_module("def f(c: bool) -> None:\n"
                                         "    if c:\n"
                                         "        def g(a: int = 1) -> int:\n"
                                         "            return a\n"
                                         "    else:\n"
                                         "        def g(a: int) -> int:\n"
                                         "            return a\n"
                                         "    print(g(1))\n");
    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "name \"g\" already defined on line 3");
    EXPECT_EQ(error.line, 6);
}

// m26/m28: a def colliding with a VARIABLE binding, which mypy reports
// regardless of conditionality -- the class the site's own comment warns must
// not be swept into the allowance.
//   $ mypy --strict ... m26_fn_var_then_cond_def.py
//   m26...py:4: error: Incompatible redefinition (redefinition with type
//                      "Callable[[], int]", original type "int")  [misc]
//   $ mypy --strict ... m28_fn_assign_then_cond_def.py  -> same message, line 4
TEST(TypeChecker, AConditionalNestedDefCollidingWithAVariableStillCollides) {
    const Checked annotated = check_module("def f(c: bool) -> None:\n"
                                           "    g: int = 1\n"
                                           "    if c:\n"
                                           "        def g() -> int:\n"
                                           "            return 0\n"
                                           "    print(g)\n");
    const diagnostics::Diagnostic annotated_error = only_error(annotated);
    EXPECT_EQ(annotated_error.code, "TypeError");
    EXPECT_EQ(annotated_error.message, "name \"g\" already defined on line 2");
    EXPECT_EQ(annotated_error.line, 4);

    const Checked inferred = check_module("def f(c: bool) -> None:\n"
                                          "    g = 1\n"
                                          "    if c:\n"
                                          "        def g() -> int:\n"
                                          "            return 0\n"
                                          "    print(g)\n");
    const diagnostics::Diagnostic inferred_error = only_error(inferred);
    EXPECT_EQ(inferred_error.code, "TypeError");
    EXPECT_EQ(inferred_error.message, "name \"g\" already defined on line 2");
    EXPECT_EQ(inferred_error.line, 4);
}

// m27: the reverse order -- a conditional def, then a conditional VARIABLE.
// The later definition is not a def, so the allowance does not apply.
//   $ mypy --strict ... m27_fn_cond_def_then_cond_var.py
//   m27...py:6: error: Name "g" already defined on line 3  [no-redef]
TEST(TypeChecker, AConditionalVariableAfterAConditionalNestedDefStillCollides) {
    const Checked checked = check_module("def f(c: bool) -> None:\n"
                                         "    if c:\n"
                                         "        def g() -> int:\n"
                                         "            return 0\n"
                                         "    if not c:\n"
                                         "        g: int = 1\n"
                                         "    print(g)\n");
    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "name \"g\" already defined on line 3");
    EXPECT_EQ(error.line, 6);
}

// m01: two flat MODULE-level defs -- the other half of the rule, unchanged by
// this round and pinned so the alignment cannot silence it.
//   $ mypy --strict ... m01_mod_flat.py
//   m01_mod_flat.py:3: error: Name "g" already defined on line 1  [no-redef]
TEST(TypeChecker, TwoFlatModuleLevelDefsStillCollide) {
    const Checked checked = check_module("def g() -> int:\n"
                                         "    return 0\n"
                                         "def g() -> int:\n"
                                         "    return 1\n"
                                         "print(g())\n");
    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "name \"g\" already defined on line 1");
    EXPECT_EQ(error.line, 3);
}

// A CLASS redefinition. mypy allows no conditional class redefinition at all,
// and this round's allowance must not reach it.
//   $ mypy --strict ... c_classredef.py
//   c_classredef.py:3: error: Name "D" already defined on line 1  [no-redef]
//   $ mypy --strict ... m18_mod_cond_class.py
//   m18...py:6: error: Name "K" already defined on line 3  [no-redef]
TEST(TypeChecker, AClassRedefinitionStillCollidesFlatAndConditional) {
    const Checked flat = check_module("class D:\n"
                                      "    a: int = 0\n"
                                      "class D:\n"
                                      "    b: int = 0\n"
                                      "print(D)\n");
    const diagnostics::Diagnostic flat_error = only_error(flat);
    EXPECT_EQ(flat_error.code, "TypeError");
    EXPECT_EQ(flat_error.message, "name \"D\" already defined on line 1");
    EXPECT_EQ(flat_error.line, 3);

    const Checked conditional = check_module("C: bool = True\n"
                                             "if C:\n"
                                             "    class K:\n"
                                             "        a: int = 0\n"
                                             "else:\n"
                                             "    class K:\n"
                                             "        b: int = 0\n"
                                             "print(K)\n");
    const diagnostics::Diagnostic conditional_error = only_error(conditional);
    EXPECT_EQ(conditional_error.code, "TypeError");
    EXPECT_EQ(conditional_error.message, "name \"K\" already defined on line 3");
    EXPECT_EQ(conditional_error.line, 6);
}

// m33/m34: the MODULE-scope half of the signature-equality test, which was a
// false NEGATIVE before this round -- mypy rejects, cythonpp was silent. The
// two sites now apply one rule, which is why this is pinned here rather than
// disclosed.
//   $ mypy --strict ... m33_mod_cond_diff_sig.py
//   m33...py:6: error: All conditional function variants must have identical
//                      signatures  [misc]
//   $ mypy --strict ... m34_mod_flat_then_cond_diff_sig.py  -> same, line 5
TEST(TypeChecker, AConditionalModuleLevelDefWithADifferentSignatureCollides) {
    const Checked both_arms = check_module("C: bool = True\n"
                                           "if C:\n"
                                           "    def g() -> int:\n"
                                           "        return 0\n"
                                           "else:\n"
                                           "    def g(a: int) -> str:\n"
                                           "        return \"s\"\n"
                                           "print(g)\n");
    const diagnostics::Diagnostic both_error = only_error(both_arms);
    EXPECT_EQ(both_error.code, "TypeError");
    EXPECT_EQ(both_error.message, "name \"g\" already defined on line 3");
    EXPECT_EQ(both_error.line, 6);

    const Checked flat_first = check_module("C: bool = True\n"
                                            "def g() -> int:\n"
                                            "    return 0\n"
                                            "if C:\n"
                                            "    def g(a: int) -> str:\n"
                                            "        return \"s\"\n"
                                            "print(g)\n");
    const diagnostics::Diagnostic flat_error = only_error(flat_first);
    EXPECT_EQ(flat_error.code, "TypeError");
    EXPECT_EQ(flat_error.message, "name \"g\" already defined on line 2");
    EXPECT_EQ(flat_error.line, 5);
}

// The module-scope shapes mypy ACCEPTS, so the equality test added there
// cannot have narrowed the allowance: m02 (if/else), m08 (flat then
// conditional), m24 (a `while` body), m35 (an alias, then a conditional def).
// All four: `mypy --strict` exit 0, no output.
TEST(TypeChecker, AConditionalModuleLevelDefWithTheSameSignatureIsStillClean) {
    expect_clean("C: bool = True\n"
                 "if C:\n"
                 "    def g() -> int:\n"
                 "        return 0\n"
                 "else:\n"
                 "    def g() -> int:\n"
                 "        return 1\n"
                 "print(g())\n");
    expect_clean("C: bool = True\n"
                 "def g() -> int:\n"
                 "    return 0\n"
                 "if C:\n"
                 "    def g() -> int:\n"
                 "        return 1\n"
                 "print(g())\n");
    expect_clean("C: bool = True\n"
                 "while C:\n"
                 "    def g() -> int:\n"
                 "        return 0\n"
                 "    def g() -> int:\n"
                 "        return 1\n"
                 "    break\n"
                 "print(g())\n");
    expect_clean("C: bool = True\n"
                 "def h() -> int:\n"
                 "    return 2\n"
                 "g = h\n"
                 "if C:\n"
                 "    def g() -> int:\n"
                 "        return 0\n"
                 "print(g())\n");
}

// ---------------------------------------------------------------------------
// The conditional-`def` allowance, second half: mypy's "identical signatures"
// is NOT Type::operator==. Implementing it with == was wrong in BOTH
// directions at once -- == is union-order-SENSITIVE where mypy is not, and
// carries no parameter NAMES where mypy compares them. The tests below pin
// each direction with fixtures that differ in EXACTLY the dimension under
// test, because the previous round's controls all differed in a way `Type`
// does represent and so were no evidence about either dimension.
//
// Every mypy/CPython claim below was measured on 2026-09-11 with mypy 1.18.1
// and CPython 3.14.2, on the exact fixture quoted, with the commands
// `mypy --strict --no-color-output --no-error-summary FILE` and
// `python FILE`.
// ---------------------------------------------------------------------------

// Finding 1, the MODULE-scope false positive this predicate closes. The two
// arms differ only in the ORDER of one union's members.
//   $ mypy --strict ... t1_mod_order.py   -> exit 0, no output
//   $ python t1_mod_order.py              -> `1`, exit 0
// The control that isolates the dimension is not a second fixture but
// neutering: restore `==` at collect_signatures' arm and this reports
// `6:5 TypeError: name "g" already defined on line 3`.
TEST(TypeChecker, AConditionalModuleLevelDefDifferingOnlyInUnionOrderIsClean) {
    expect_clean("C: bool = True\n"
                 "if C:\n"
                 "    def g(a: int | str) -> None:\n"
                 "        print(a)\n"
                 "else:\n"
                 "    def g(a: str | int) -> None:\n"
                 "        print(a)\n"
                 "g(1)\n");
}

// Finding 1's four FUNCTION-scope sub-shapes, all mypy exit 0 / CPython exit
// 0 as measured, and all reported before this round -- the fourth puts the
// differently-ordered union in the RETURN type rather than a parameter, which
// is a separate position in `Type::args` and so a separate chance to get the
// recursion wrong.
TEST(TypeChecker, AConditionalNestedDefDifferingOnlyInUnionOrderIsClean) {
    // Both arms of an if/else.
    expect_clean("def f(c: bool) -> None:\n"
                 "    if c:\n"
                 "        def g(a: int | str) -> None:\n"
                 "            print(a)\n"
                 "    else:\n"
                 "        def g(a: str | int) -> None:\n"
                 "            print(a)\n"
                 "    g(1)\n");
    // A FLAT def first, then a conditional one.
    expect_clean("def f(c: bool) -> None:\n"
                 "    def g(a: int | str) -> None:\n"
                 "        print(a)\n"
                 "    if c:\n"
                 "        def g(a: str | int) -> None:\n"
                 "            print(a)\n"
                 "    g(1)\n");
    // `int | None` against `None | int`, the optional spelling.
    expect_clean("def f(c: bool) -> None:\n"
                 "    if c:\n"
                 "        def g(a: int | None) -> None:\n"
                 "            print(a)\n"
                 "    else:\n"
                 "        def g(a: None | int) -> None:\n"
                 "            print(a)\n"
                 "    g(1)\n");
    // The union is the RETURN type, not a parameter.
    expect_clean("def f(c: bool) -> None:\n"
                 "    if c:\n"
                 "        def g() -> int | str:\n"
                 "            return 0\n"
                 "    else:\n"
                 "        def g() -> str | int:\n"
                 "            return \"s\"\n"
                 "    print(g())\n");
}

// Order-insensitivity has to reach INSIDE type arguments, at any depth, and
// that is a measurement rather than an assumption: every fixture below is
// `mypy --strict` exit 0 (measured one by one), and a top-level-only
// comparison would have left all seven a false positive.
TEST(TypeChecker, SignatureIdentityIsUnionOrderInsensitiveInsideTypeArguments) {
    // Inside a `list` argument.
    expect_clean("def f(c: bool) -> None:\n"
                 "    if c:\n"
                 "        def g(a: list[int | str]) -> None:\n"
                 "            print(a)\n"
                 "    else:\n"
                 "        def g(a: list[str | int]) -> None:\n"
                 "            print(a)\n"
                 "    g([])\n");
    // Inside a `dict`'s KEY position, with a second argument that matches.
    expect_clean("def f(c: bool) -> None:\n"
                 "    if c:\n"
                 "        def g(a: dict[int | str, bool]) -> None:\n"
                 "            print(a)\n"
                 "    else:\n"
                 "        def g(a: dict[str | int, bool]) -> None:\n"
                 "            print(a)\n"
                 "    g({})\n");
    // Inside a `tuple` element.
    expect_clean("def f(c: bool) -> None:\n"
                 "    if c:\n"
                 "        def g(a: tuple[int | str, bool]) -> None:\n"
                 "            print(a)\n"
                 "    else:\n"
                 "        def g(a: tuple[str | int, bool]) -> None:\n"
                 "            print(a)\n"
                 "    g((1, True))\n");
    // TWO levels deep, with both unions reordered.
    expect_clean("def f(c: bool) -> None:\n"
                 "    if c:\n"
                 "        def g(a: list[dict[int | str, list[bool | float]]]) -> None:\n"
                 "            print(a)\n"
                 "    else:\n"
                 "        def g(a: list[dict[str | int, list[float | bool]]]) -> None:\n"
                 "            print(a)\n"
                 "    g([])\n");
    // A three-member union ROTATED, so no pairwise-swap shortcut suffices.
    expect_clean("def f(c: bool) -> None:\n"
                 "    if c:\n"
                 "        def g(a: int | str | float) -> None:\n"
                 "            print(a)\n"
                 "    else:\n"
                 "        def g(a: float | int | str) -> None:\n"
                 "            print(a)\n"
                 "    g(1)\n");
    // A PARENTHESISED nested union against a flat one -- Type::union_of
    // flattens, so the two are the same three members in different orders.
    expect_clean("def f(c: bool) -> None:\n"
                 "    if c:\n"
                 "        def g(a: int | (str | float)) -> None:\n"
                 "            print(a)\n"
                 "    else:\n"
                 "        def g(a: float | str | int) -> None:\n"
                 "            print(a)\n"
                 "    g(1)\n");
    // A DUPLICATED member against a two-member union -- union_of also
    // de-duplicates, so the arity ends up equal as well as the membership.
    expect_clean("def f(c: bool) -> None:\n"
                 "    if c:\n"
                 "        def g(a: int | int | str) -> None:\n"
                 "            print(a)\n"
                 "    else:\n"
                 "        def g(a: str | int) -> None:\n"
                 "            print(a)\n"
                 "    g(1)\n");
}

// Finding 2, the FUNCTION-scope false negative this predicate closes: the two
// signatures differ only in a parameter NAME, which mypy compares and
// `Type::callable` does not carry at all.
//   $ mypy --strict ... r1_fn_name_flatfirst.py
//   r1...py:5: error: All conditional function variants must have identical
//                     signatures  [misc]
//   r1...py:5: note:     def g(a: int) -> int   /   def g(b: int) -> int
//   $ python r1_fn_name_flatfirst.py  -> exit 0   (CPython accepts; the union
//                                         rule still forbids accepting it)
// Measured the same way for all four shapes below, each at the line asserted.
TEST(TypeChecker, AConditionalNestedDefDifferingOnlyInAParameterNameCollides) {
    const Checked flat_first = check_module("def f(c: bool) -> None:\n"
                                            "    def g(a: int) -> int:\n"
                                            "        return a\n"
                                            "    if c:\n"
                                            "        def g(b: int) -> int:\n"
                                            "            return b\n"
                                            "    print(g(1))\n");
    const diagnostics::Diagnostic flat_error = only_error(flat_first);
    EXPECT_EQ(flat_error.code, "TypeError");
    EXPECT_EQ(flat_error.message, "name \"g\" already defined on line 2");
    EXPECT_EQ(flat_error.line, 5);

    const Checked both_arms = check_module("def f(c: bool) -> None:\n"
                                           "    if c:\n"
                                           "        def g(a: int) -> int:\n"
                                           "            return a\n"
                                           "    else:\n"
                                           "        def g(b: int) -> int:\n"
                                           "            return b\n"
                                           "    print(g(1))\n");
    const diagnostics::Diagnostic both_error = only_error(both_arms);
    EXPECT_EQ(both_error.code, "TypeError");
    EXPECT_EQ(both_error.message, "name \"g\" already defined on line 3");
    EXPECT_EQ(both_error.line, 6);

    const Checked while_body = check_module("def f(c: bool) -> None:\n"
                                            "    def g(a: int) -> int:\n"
                                            "        return a\n"
                                            "    while c:\n"
                                            "        def g(b: int) -> int:\n"
                                            "            return b\n"
                                            "        break\n"
                                            "    print(g(1))\n");
    const diagnostics::Diagnostic while_error = only_error(while_body);
    EXPECT_EQ(while_error.code, "TypeError");
    EXPECT_EQ(while_error.message, "name \"g\" already defined on line 2");
    EXPECT_EQ(while_error.line, 5);

    // Names SWAPPED between two parameters: the multiset of names and the
    // list of types are both unchanged, so only an ORDERED name comparison
    // catches it.
    const Checked swapped = check_module("def f(c: bool) -> None:\n"
                                         "    if c:\n"
                                         "        def g(a: int, b: str) -> None:\n"
                                         "            print(a, b)\n"
                                         "    else:\n"
                                         "        def g(b: int, a: str) -> None:\n"
                                         "            print(b, a)\n"
                                         "    g(1, \"s\")\n");
    const diagnostics::Diagnostic swapped_error = only_error(swapped);
    EXPECT_EQ(swapped_error.code, "TypeError");
    EXPECT_EQ(swapped_error.message, "name \"g\" already defined on line 3");
    EXPECT_EQ(swapped_error.line, 6);
}

// The MODULE-scope twin of the function-scope case above, and silent both
// before and after the fix that closed that one -- so this half was never
// closed for a name-only difference, only for a type-only one. One predicate
// at two sites closes it.
//   $ mypy --strict ... r5_mod_name.py
//   r5...py:6: error: All conditional function variants must have identical
//                     signatures  [misc]
//   $ python r5_mod_name.py -> `1`, exit 0
TEST(TypeChecker, AConditionalModuleLevelDefDifferingOnlyInAParameterNameCollides) {
    const Checked checked = check_module("C: bool = True\n"
                                         "if C:\n"
                                         "    def g(a: int) -> int:\n"
                                         "        return a\n"
                                         "else:\n"
                                         "    def g(b: int) -> int:\n"
                                         "        return b\n"
                                         "print(g(1))\n");
    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "name \"g\" already defined on line 3");
    EXPECT_EQ(error.line, 6);
}

// Both dimensions at once, which is the one fixture that would pass under
// EITHER half of the fix alone but only for the wrong reason: a predicate
// that fixed order and forgot names would silence it. mypy reports it.
//   $ mypy --strict ... r6_fn_name_and_order.py
//   r6...py:6: error: All conditional function variants must have identical
//                     signatures  [misc]
//   r6...py:6: note:     def g(a: int | str) -> None
//   r6...py:6: note:     def g(b: str | int) -> None
TEST(TypeChecker, AConditionalNestedDefDifferingInBothAParameterNameAndUnionOrderCollides) {
    const Checked checked = check_module("def f(c: bool) -> None:\n"
                                         "    if c:\n"
                                         "        def g(a: int | str) -> None:\n"
                                         "            print(a)\n"
                                         "    else:\n"
                                         "        def g(b: str | int) -> None:\n"
                                         "            print(b)\n"
                                         "    g(1)\n");
    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "name \"g\" already defined on line 3");
    EXPECT_EQ(error.line, 6);
}

// Two differences the order-insensitive comparison must NOT swallow, both
// mypy `All conditional function variants must have identical signatures
// [misc]` as measured: a differing parameter COUNT (which also differs in
// `args.size()`, the guard the unordered union match depends on) and a
// differing type NESTED inside an argument (which the recursion must
// distinguish from a reordering at that same depth).
TEST(TypeChecker, AConditionalNestedDefDifferingInParameterCountOrANestedTypeCollides) {
    const Checked count = check_module("def f(c: bool) -> None:\n"
                                       "    if c:\n"
                                       "        def g(a: int) -> None:\n"
                                       "            print(a)\n"
                                       "    else:\n"
                                       "        def g(a: int, b: int) -> None:\n"
                                       "            print(a, b)\n"
                                       "    g(1)\n");
    const diagnostics::Diagnostic count_error = only_error(count);
    EXPECT_EQ(count_error.code, "TypeError");
    EXPECT_EQ(count_error.message, "name \"g\" already defined on line 3");
    EXPECT_EQ(count_error.line, 6);

    const Checked nested = check_module("def f(c: bool) -> None:\n"
                                        "    if c:\n"
                                        "        def g(a: list[int]) -> None:\n"
                                        "            print(a)\n"
                                        "    else:\n"
                                        "        def g(a: list[str]) -> None:\n"
                                        "            print(a)\n"
                                        "    g([])\n");
    const diagnostics::Diagnostic nested_error = only_error(nested);
    EXPECT_EQ(nested_error.code, "TypeError");
    EXPECT_EQ(nested_error.message, "name \"g\" already defined on line 3");
    EXPECT_EQ(nested_error.line, 6);
}

// The unordered union match must consume each member at most once, not just
// test one-way containment at equal sizes. Type::union_of de-duplicates with
// `operator==`, which is order-SENSITIVE, so `list[int | str] |
// list[str | int]` really does survive as a TWO-member union whose members
// are identical under this predicate -- and against `list[int | str] | int`,
// also two members, containment alone matches both left members onto the
// first right one and never notices the `int`. Measured: with containment
// only, this fixture goes silent.
//   $ mypy --strict ... u1.py
//   u1.py:6: error: All conditional function variants must have identical
//                   signatures  [misc]
//   u1.py:6: note:     def g(a: list[int | str]) -> None
//   u1.py:6: note:     def g(a: list[int | str] | int) -> None
//   $ python u1.py -> exit 0
// (mypy collapses the repeated member where Type::union_of does not; both
// tools reject the program either way, so that is not a divergence here.)
TEST(TypeChecker, AUnionRepeatingOneMemberStillNoticesADifferentMember) {
    const Checked checked =
        check_module("def f(c: bool) -> None:\n"
                     "    if c:\n"
                     "        def g(a: list[int | str] | list[str | int]) -> None:\n"
                     "            print(a)\n"
                     "    else:\n"
                     "        def g(a: list[int | str] | int) -> None:\n"
                     "            print(a)\n"
                     "    print(g)\n");
    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "name \"g\" already defined on line 3");
    EXPECT_EQ(error.line, 6);
}

// An alias of a function value with a PARAMETER, then a conditional `def` of
// the same name whose parameter name MATCHES the aliased function's. The
// existing binding carries a Callable type but no recorded names, and this is
// the fixture that fixes which way that ambiguity must resolve: mypy accepts
// it, so "names not recorded" must fall back to comparing types only.
//   $ mypy --strict ... m31q.py  -> exit 0, no output
//   $ python m31q.py             -> `1`, exit 0
// The opposite default is not hypothetical: with a DIFFERENT parameter name
// the same shape is a mypy error this compiler does not report (disclosed as
// a missed error, the safe direction -- closing it needs parameter names
// inside `Type`, which the header explains was rejected), and "unknown names
// means report" would have turned THIS fixture into a false positive to
// close it.
TEST(TypeChecker, AConditionalNestedDefOverAnAliasWithAParameterIsStillClean) {
    expect_clean("def h(x: int) -> int:\n"
                 "    return x\n"
                 "def f(c: bool) -> None:\n"
                 "    g = h\n"
                 "    if c:\n"
                 "        def g(x: int) -> int:\n"
                 "            return x\n"
                 "    print(g(1))\n");
}

// All six collision classes the round-5 review's §2.2 table requires to keep
// reporting, pinned together as one guard on THIS predicate: a predicate too
// loose in any direction silences one of them. Each row re-measured at
// 2026-09-11 on the exact fixture below; mypy's code is in the comment, and
// where cythonpp's line pair is reversed relative to mypy's that is the
// long-standing cosmetic divergence (parked item (f)), not a disagreement
// about whether the program is bad.
TEST(TypeChecker, TheSixCollisionClassesThatMustKeepReportingStillDo) {
    // 1. conditional def, then a FLAT def. mypy `:5: Name "g" already
    //    defined on line 3  [no-redef]`. Re-measured 2026-09-13: this row's
    //    line pair used to be the reversed cosmetic divergence the class
    //    comment describes (reported at line 3, "already defined on line 5"),
    //    because pre_bind_function_body did not recurse into the `if` body
    //    that declares the conditional `def` at line 3, so only the FLAT def
    //    at line 5 was ever pre-bound and the conditional one collided
    //    against IT when the real walk reached it first. Now that the
    //    pre-bind pass recurses into If/While/For bodies (closing Defect B),
    //    the conditional def at line 3 is pre-bound first (textual order),
    //    the real walk fills that placeholder in cleanly, and the FLAT def at
    //    line 5 is the one that collides -- matching mypy's own line pair
    //    exactly, not merely still reporting.
    const Checked cond_then_flat = check_module("def f(c: bool) -> None:\n"
                                                "    if c:\n"
                                                "        def g() -> int:\n"
                                                "            return 0\n"
                                                "    def g() -> int:\n"
                                                "        return 1\n"
                                                "    print(g())\n");
    const diagnostics::Diagnostic cond_then_flat_error = only_error(cond_then_flat);
    EXPECT_EQ(cond_then_flat_error.code, "TypeError");
    EXPECT_EQ(cond_then_flat_error.message, "name \"g\" already defined on line 3");
    EXPECT_EQ(cond_then_flat_error.line, 5);

    // 2. def, then a VARIABLE. mypy `:4: Name "g" already defined on line 2
    //    [no-redef]`.
    const Checked def_then_var = check_module("def f(c: bool) -> None:\n"
                                              "    def g() -> int:\n"
                                              "        return 1\n"
                                              "    g: int = 2\n"
                                              "    print(g)\n");
    const diagnostics::Diagnostic def_then_var_error = only_error(def_then_var);
    EXPECT_EQ(def_then_var_error.code, "TypeError");
    EXPECT_EQ(def_then_var_error.message, "name \"g\" already defined on line 2");
    EXPECT_EQ(def_then_var_error.line, 4);

    // 3. VARIABLE, then a conditional def. mypy `:4: Incompatible
    //    redefinition (redefinition with type "Callable[[], int]", original
    //    type "int")  [misc]`. has_identical_signature rejects it on `kind`
    //    (Int is not Callable) rather than on its explicit non-Callable
    //    guard, which is defensive only -- measured, deleting that guard
    //    fails no test.
    const Checked var_then_cond = check_module("def f(c: bool) -> None:\n"
                                               "    g: int = 2\n"
                                               "    if c:\n"
                                               "        def g() -> int:\n"
                                               "            return 1\n"
                                               "    print(g)\n");
    const diagnostics::Diagnostic var_then_cond_error = only_error(var_then_cond);
    EXPECT_EQ(var_then_cond_error.code, "TypeError");
    EXPECT_EQ(var_then_cond_error.message, "name \"g\" already defined on line 2");
    EXPECT_EQ(var_then_cond_error.line, 4);

    // 4. two FLAT module defs. mypy `:3: Name "g" already defined on line 1
    //    [no-redef]`.
    const Checked two_flat = check_module("def g() -> int:\n"
                                          "    return 1\n"
                                          "def g() -> int:\n"
                                          "    return 2\n"
                                          "print(g())\n");
    const diagnostics::Diagnostic two_flat_error = only_error(two_flat);
    EXPECT_EQ(two_flat_error.code, "TypeError");
    EXPECT_EQ(two_flat_error.message, "name \"g\" already defined on line 1");
    EXPECT_EQ(two_flat_error.line, 3);

    // 5. a CLASS redefined in both arms of an if/else. mypy `:6: Name "D"
    //    already defined on line 3  [no-redef]` -- the allowance is for
    //    `def`s only, at every scope.
    const Checked cond_class = check_module("C: bool = True\n"
                                            "if C:\n"
                                            "    class D:\n"
                                            "        a: int = 0\n"
                                            "else:\n"
                                            "    class D:\n"
                                            "        b: int = 1\n"
                                            "print(D)\n");
    const diagnostics::Diagnostic cond_class_error = only_error(cond_class);
    EXPECT_EQ(cond_class_error.code, "TypeError");
    EXPECT_EQ(cond_class_error.message, "name \"D\" already defined on line 3");
    EXPECT_EQ(cond_class_error.line, 6);

    // 6. conditional defs whose signatures differ in TYPE. mypy `:6: All
    //    conditional function variants must have identical signatures
    //    [misc]`.
    const Checked type_differs = check_module("def f(c: bool) -> None:\n"
                                              "    if c:\n"
                                              "        def g(a: int) -> None:\n"
                                              "            print(a)\n"
                                              "    else:\n"
                                              "        def g(a: str) -> None:\n"
                                              "            print(a)\n"
                                              "    g(1)\n");
    const diagnostics::Diagnostic type_differs_error = only_error(type_differs);
    EXPECT_EQ(type_differs_error.code, "TypeError");
    EXPECT_EQ(type_differs_error.message, "name \"g\" already defined on line 3");
    EXPECT_EQ(type_differs_error.line, 6);
}

// A loop whose body has no reachable break ALWAYS runs its else, so an else
// that returns makes the whole loop return. Measured 2026-09-12: mypy
// --strict is Success and CPython prints 1 then 3.
TEST(TypeChecker, AForElseThatReturnsIsNotAMissingReturn) {
    expect_clean("def f(xs: list[int]) -> int:\n"
                 "    for x in xs:\n"
                 "        print(x)\n"
                 "    else:\n"
                 "        return 3\n");
}

TEST(TypeChecker, AWhileElseThatReturnsIsNotAMissingReturn) {
    expect_clean("def f(c: bool) -> int:\n"
                 "    while c:\n"
                 "        print(1)\n"
                 "    else:\n"
                 "        return 3\n");
}

// A8: the break belongs to the INNER loop and can never escape the outer
// one, so the outer else still always runs. mypy: Success.
TEST(TypeChecker, ABreakInANestedLoopBodyDoesNotBlockTheOuterElse) {
    expect_clean("def f(xs: list[int], ys: list[int]) -> int:\n"
                 "    for x in xs:\n"
                 "        for y in ys:\n"
                 "            break\n"
                 "    else:\n"
                 "        return 3\n");
}

// A10: the else returns through both arms of an if.
TEST(TypeChecker, ALoopElseReturningViaBothIfArmsIsClean) {
    expect_clean("def f(xs: list[int], c: bool) -> int:\n"
                 "    for x in xs:\n"
                 "        print(x)\n"
                 "    else:\n"
                 "        if c:\n"
                 "            return 1\n"
                 "        else:\n"
                 "            return 2\n");
}

// A24: the predicate is "SOME statement always returns", not "the last one
// does" -- a non-returning statement after the loop stays clean.
TEST(TypeChecker, AStatementAfterAReturningLoopElseIsStillClean) {
    expect_clean("def f(xs: list[int]) -> int:\n"
                 "    for x in xs:\n"
                 "        print(x)\n"
                 "    else:\n"
                 "        return 3\n"
                 "    print(1)\n");
}

// A20: continue does not skip the else.
TEST(TypeChecker, AContinueDoesNotBlockTheLoopElse) {
    expect_clean("def f(xs: list[int]) -> int:\n"
                 "    for x in xs:\n"
                 "        continue\n"
                 "    else:\n"
                 "        return 3\n");
}

// A14: a break after a return in the same block is unreachable, so it does
// not make the else skippable. mypy prunes it; so must the break scan.
TEST(TypeChecker, AnUnreachableBreakAfterAReturnDoesNotBlockTheLoopElse) {
    expect_clean("def f(xs: list[int]) -> int:\n"
                 "    for x in xs:\n"
                 "        return 1\n"
                 "        break\n"
                 "    else:\n"
                 "        return 3\n");
}

// A2: an unconditional break escapes the loop and skips the else, so
// control can fall off the end. Differs from the A1 clean case ONLY in the
// presence of the break.
TEST(TypeChecker, AReachableBreakMakesAReturningLoopElseAMissingReturn) {
    const Checked checked = check_module("def f(xs: list[int]) -> int:\n"
                                         "    for x in xs:\n"
                                         "        break\n"
                                         "    else:\n"
                                         "        return 3\n");

    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "missing return statement");
    EXPECT_EQ(error.line, 1);
}

// A3: a CONDITIONAL break is still a reachable break. This is the control
// that differs from the `if False: break` shape in exactly one dimension --
// whether the condition is a prunable constant.
TEST(TypeChecker, AConditionalBreakStillBlocksTheLoopElse) {
    const Checked checked = check_module("def f(xs: list[int], c: bool) -> int:\n"
                                         "    for x in xs:\n"
                                         "        if c:\n"
                                         "            break\n"
                                         "    else:\n"
                                         "        return 3\n");

    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "missing return statement");
    EXPECT_EQ(error.line, 1);
}

// A9: a break in a NESTED loop's ELSE belongs to the OUTER loop -- the
// exact inverse of A8 above, differing only in body-vs-else placement.
TEST(TypeChecker, ABreakInANestedLoopElseBlocksTheOuterElse) {
    const Checked checked = check_module("def f(xs: list[int], ys: list[int]) -> int:\n"
                                         "    for x in xs:\n"
                                         "        for y in ys:\n"
                                         "            pass\n"
                                         "        else:\n"
                                         "            break\n"
                                         "    else:\n"
                                         "        return 3\n");

    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "missing return statement");
    EXPECT_EQ(error.line, 1);
}

// A7: the clause reads ORELSE only. A `for` BODY may run zero times, so a
// returning body with no else is still a missing return.
TEST(TypeChecker, AReturningForBodyWithNoElseIsStillAMissingReturn) {
    const Checked checked = check_module("def f(xs: list[int]) -> int:\n"
                                         "    for x in xs:\n"
                                         "        return 1\n");

    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "missing return statement");
    EXPECT_EQ(error.line, 1);
}

// A6: an else that does NOT return keeps the diagnostic. Differs from the
// A1 clean case only in whether the else returns.
TEST(TypeChecker, ALoopElseThatDoesNotReturnIsStillAMissingReturn) {
    const Checked checked = check_module("def f(xs: list[int]) -> int:\n"
                                         "    for x in xs:\n"
                                         "        print(x)\n"
                                         "    else:\n"
                                         "        print(1)\n");

    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "missing return statement");
    EXPECT_EQ(error.line, 1);
}

// A12/A18/A19: a break inside an if's else arm still belongs to the loop.
TEST(TypeChecker, ABreakInAnIfElseArmBlocksTheLoopElse) {
    const Checked checked = check_module("def f(xs: list[int], c: bool) -> int:\n"
                                         "    for x in xs:\n"
                                         "        if c:\n"
                                         "            print(1)\n"
                                         "        else:\n"
                                         "            break\n"
                                         "    else:\n"
                                         "        return 3\n");

    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "missing return statement");
    EXPECT_EQ(error.line, 1);
}

// contains_reachable_break's break scan is reached from
// statement_always_leaves's While/For arms, which check_suite calls at
// MODULE and CLASS scope too, not only from a FunctionDef's own body. A
// `return` at module scope is not legal Python at all (mypy: `"return"
// outside function [misc]`; CPython: `SyntaxError: 'return' outside
// function`), so it must not be treated as "always leaves" there. Measured
// 2026-09-12, second round: hardcoding in_function=true in that scan once
// made `while True: return / break` at module scope silently swallow the
// diagnostic for the incompatible assignment that follows it.
TEST(TypeChecker, AReturnAtModuleScopeDoesNotSuppressALaterAssignmentError) {
    const Checked checked = check_module("while True:\n"
                                         "    return\n"
                                         "    break\n"
                                         "x: int = \"s\"\n");

    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message,
              "incompatible types in assignment (expression has type \"str\", variable has "
              "type \"int\")");
    EXPECT_EQ(error.line, 4);
}

// An if/else where BOTH arms return still makes the trailing break dead
// code. mypy: Success. CPython prints 1.
TEST(TypeChecker, AnIfWhereBothArmsReturnDoesNotBlockTheLoopElse) {
    expect_clean("def f(xs: list[int], c: bool) -> int:\n"
                 "    for x in xs:\n"
                 "        if c:\n"
                 "            return 1\n"
                 "        else:\n"
                 "            return 2\n"
                 "        break\n"
                 "    else:\n"
                 "        return 3\n");
}

// A nested `while True: pass` with no break of its own never falls through
// to the trailing break. mypy: Success.
TEST(TypeChecker, ANestedWhileTrueWithNoBreakDoesNotBlockTheLoopElse) {
    expect_clean("def f(xs: list[int]) -> int:\n"
                 "    for x in xs:\n"
                 "        while True:\n"
                 "            pass\n"
                 "        break\n"
                 "    else:\n"
                 "        return 3\n");
}

// A nested `for ... else: return` guarantees its own return before the
// trailing break can ever run. mypy: Success.
TEST(TypeChecker, ANestedForElseThatReturnsDoesNotBlockTheOuterLoopElse) {
    expect_clean("def f(xs: list[int], ys: list[int]) -> int:\n"
                 "    for x in xs:\n"
                 "        for y in ys:\n"
                 "            pass\n"
                 "        else:\n"
                 "            return 1\n"
                 "        break\n"
                 "    else:\n"
                 "        return 3\n");
}

// An if/else where one arm returns and the other continues also always
// leaves, so the trailing break is dead too. mypy: Success.
TEST(TypeChecker, AnIfReturningOrContinuingDoesNotBlockTheLoopElse) {
    expect_clean("def f(xs: list[int], c: bool) -> int:\n"
                 "    for x in xs:\n"
                 "        if c:\n"
                 "            return 1\n"
                 "        else:\n"
                 "            continue\n"
                 "        break\n"
                 "    else:\n"
                 "        return 3\n");
}

// CONTROL: an `if` with NO `else`, even though its one arm returns, does not
// always leave (an `if` with no `else` can always fall through), so the
// trailing break IS still reachable and must still report. mypy agrees:
// `Missing return statement`.
TEST(TypeChecker, AnIfWithNoElseStillLeavesTheTrailingBreakReachable) {
    const Checked checked = check_module("def f(xs: list[int], c: bool) -> int:\n"
                                         "    for x in xs:\n"
                                         "        if c:\n"
                                         "            return 1\n"
                                         "        break\n"
                                         "    else:\n"
                                         "        return 3\n");

    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "missing return statement");
    EXPECT_EQ(error.line, 1);
}

// Every other reachable-break control above used a `for`; this pins the
// While arm's own break-suppression independently, not just its clean case
// (AWhileElseThatReturnsIsNotAMissingReturn, above). mypy agrees: `Missing
// return statement`.
TEST(TypeChecker, AWhileReachableBreakMakesAReturningLoopElseAMissingReturn) {
    const Checked checked = check_module("def f(c: bool) -> int:\n"
                                         "    while c:\n"
                                         "        break\n"
                                         "    else:\n"
                                         "        return 3\n");

    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "missing return statement");
    EXPECT_EQ(error.line, 1);
}

// ---------------------------------------------------------------------------
// Builtin constructor arities. Ten violations, one root cause: the generated
// class table recorded no arity at all, so a builtin with no __init__ and no
// base defaulted to a zero-arg Callable (reporting a false "too many
// arguments"), while an exception class reached constructor_check's
// BaseException arm and went Unchecked (silently accepting a bad call).
// Measurements 2026-09-12, mypy 1.18.1 (compiled: yes) / Python 3.14.2.
// ---------------------------------------------------------------------------

// Five false positives: mypy --strict is clean and CPython runs each of
// these, while this checker reported `too many arguments`. The generated
// table recorded no arity, so a builtin with no __init__ defaulted to
// zero-arg.
TEST(TypeChecker, BuiltinConstructorsWithRealAritiesAreNotTooManyArguments) {
    expect_clean("memoryview(b\"ab\")\n");
    expect_clean("property(None)\n");
    expect_clean("slice(1)\n");
    expect_clean("staticmethod(len)\n");
    expect_clean("type(1)\n");
}

// Five false negatives: BOTH oracles reject each of these and this checker
// was silent, because an exception class reached constructor_check's
// BaseException arm and returned Unchecked.
TEST(TypeChecker, AnExceptionGroupWithTooFewArgumentsIsReported) {
    const Checked checked = check_module("ExceptionGroup(1)\n");

    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "too few arguments for \"ExceptionGroup\"");
    EXPECT_EQ(error.line, 1);
}

TEST(TypeChecker, AUnicodeDecodeErrorWithTooFewArgumentsIsReported) {
    const Checked checked = check_module("UnicodeDecodeError(1)\n");

    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "too few arguments for \"UnicodeDecodeError\"");
    EXPECT_EQ(error.line, 1);
}

// The band has an upper edge too: UnicodeTranslateError is (4, 4).
TEST(TypeChecker, AUnicodeTranslateErrorWithTooManyArgumentsIsReported) {
    const Checked checked =
        check_module("UnicodeTranslateError(1, 2, 3, 4, 5)\n");

    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "too many arguments for \"UnicodeTranslateError\"");
    EXPECT_EQ(error.line, 1);
}

// CONTROL: an unbounded row must stay unconstrained. ValueError takes *args
// and both oracles accept any count -- this differs from the rows above in
// exactly one dimension, whether the row is bounded.
TEST(TypeChecker, AnUnboundedBuiltinConstructorIsStillUnchecked) {
    expect_clean("ValueError(1, 2, 3)\n");
    expect_clean("OSError()\n");
}

// `type` accepts exactly {1, 3} and BOTH oracles reject arity 2 -- a hole no
// (min, max) band can represent. Under the [call-arg] filter `type` reads
// unbounded, so this stays silent: a MISSED ERROR in the safe direction, not
// a false positive. Pinned so the choice is visible rather than accidental.
TEST(TypeChecker, AnOverloadedBuiltinConstructorArityIsDeliberatelyUnchecked) {
    expect_clean("type(1, 2)\n");
}

// slice and type read UNBOUNDED under the [call-arg] filter, so seeding
// bounded rows alone does NOT fix them -- they fall through to the zero-arg
// default. Both are mypy Success and both run clean under CPython.
TEST(TypeChecker, AnUnboundedBuiltinConstructorDoesNotDefaultToZeroArg) {
    expect_clean("slice(1)\n");
    expect_clean("type(1)\n");
}

// CONTROL: object is BOUNDED (0, 0), so it must keep reporting. Differs from
// the two above in exactly one dimension -- whether the row is bounded. Both
// oracles reject object(1).
TEST(TypeChecker, AZeroArgBoundedBuiltinStillReportsTooManyArguments) {
    const Checked checked = check_module("object(1)\n");

    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "too many arguments for \"object\"");
    EXPECT_EQ(error.line, 1);
}

// PINS the "consulted last" contract directly: `ClassTable::constructor_check`
// must check a seeded row's own arity band ONLY after a declared __init__, a
// builtin-kind base and a declared __new__ have all already missed. `complex`
// is an UNBOUNDED row (like `slice`/`type`), but unlike them it is also a
// model KIND (TypeKind::Complex), so its own entry hits the BuiltinKindBase
// arm immediately -- BEFORE the band is ever consulted -- and the answer must
// stay the sanctioned `NotImplementedError`, never silence. Moving the band
// lookup any earlier (even to the very top of the function) flips this: an
// unbounded row would answer Unchecked before BuiltinKindBase ever runs,
// silently accepting `complex(1)`, which is a genuinely bounded overload set
// neither oracle would accept at every arity.
TEST(TypeChecker, AnUnboundedModelKindConstructorStillDefersRatherThanGoingSilent) {
    const Checked checked = check_module("complex(1)\n");

    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "NotImplementedError");
    EXPECT_EQ(error.message,
              "calls to 'complex', which inherits an overloaded builtin constructor, are not "
              "supported");
    EXPECT_EQ(error.line, 1);
}

// 2026-09-16, pre_bind_assignment_targets' placeholder LINE. A module-level
// name whose first binding sits inside an `if`/`while`/`for` body and which is
// assigned again at top level used to take its DECLARED type from the LATER
// top-level assignment: the Phase-2.5 placeholder was stamped with that
// statement's line, so is_unfilled_placeholder refused to let the earlier,
// genuinely-first assignment fill it. mypy takes the declared type from the
// FIRST assignment in source order, wherever it sits, so the later
// incompatible one is an error -- measured, all three shapes
// `Incompatible types in assignment (expression has type "float", variable
// has type "int")`, and all three were SILENT here. Worse than a bare missed
// diagnostic: codegen hoists its declaration from the same first assignment,
// so it emitted `py::int_ cy_x;` and then assigned a `py::float_` into it --
// uncompilable C++ written by a run that exited 0.
TEST(TypeChecker, ABlockNestedFirstAssignmentFixesTheDeclaredType) {
    for (const std::string source :
         {// The `if` body.
          std::string("c: bool = True\nif c:\n    x = 1\nx = 2.5\nprint(x)\n"),
          // The `while` body.
          std::string("c: bool = True\nwhile c:\n    x = 1\n    c = False\nx = 2.5\n"
                      "print(x)\n"),
          // The `for` body.
          std::string("for i in [1]:\n    x = 1\nx = 2.5\nprint(x)\n"),
          // A non-numeric pair, so the rule is not read as numeric-tower
          // specific: mypy `Incompatible types in assignment (expression has
          // type "int", variable has type "str")`.
          std::string("c: bool = True\nif c:\n    s = \"a\"\ns = 5\nprint(s)\n")}) {
        const Checked checked = check_module(source);
        const diagnostics::Diagnostic error = only_error(checked);
        EXPECT_EQ(error.code, "TypeError") << source;
        EXPECT_NE(error.message.find("incompatible types in assignment"), std::string::npos)
            << source << " -> " << error.message;
    }
}

// THE FALSE POSITIVE the same placeholder line caused, and the reason this fix
// is not merely a missed-diagnostic cleanup: a read sitting BETWEEN the
// block-nested first assignment and a later top-level one reported
// `name 'x' is used before definition` against that later line. Measured
// 2026-09-16: mypy `Success`, CPython prints 1 then 2 at exit 0 -- BOTH
// oracles accept and RUN this program.
TEST(TypeChecker, AReadBetweenABlockFirstAssignmentAndALaterOneStaysClean) {
    expect_clean("c: bool = True\nif c:\n    x = 1\nprint(x)\nx = 2\nprint(x)\n");
}

// CONTROL: the compatible sibling of ABlockNestedFirstAssignmentFixesTheDeclared
// Type above. The declared type still comes from the block-nested assignment,
// so a later top-level `int` is fine -- mypy `Success`, CPython prints 2.
// Guards the direction: a fix that reported on ANY block-first/top-level pair
// rather than on an incompatible one would fail here.
TEST(TypeChecker, ABlockNestedFirstAssignmentOfACompatibleTypeStaysClean) {
    expect_clean("c: bool = True\nif c:\n    x = 1\nx = 2\nprint(x)\n");
}

// CONTROL, THE RECORDED HAZARD: widening WHICH names get a placeholder (rather
// than only the line an existing one carries) was tried before and reverted,
// because it made this exact program a false NameError -- see
// pre_bind_assignment_targets' own header comment. Measured again 2026-09-16
// at this fix: mypy `Success`, CPython prints a, b, then z at exit 0. The
// placeholder SET is deliberately unchanged, so `line` -- bound by a `for`
// target and by a nested `if`, never by a top-level assignment -- still gets
// no placeholder at all and this stays clean.
TEST(TypeChecker, AModuleLoopReadAboveALaterConditionalAssignmentStaysClean) {
    expect_clean("for line in [\"a\", \"b\"]:\n    print(line)\nif True:\n    line = \"z\"\n"
                 "print(line)\n");
}

// CONTROL, THE OTHER DIRECTION: the module-level sibling of
// ALoopCarriedAccumulatorGuardedByAFlagStaysClean must keep REPORTING, and the
// reason is the union rule rather than a preference. Measured 2026-09-16:
// mypy `Cannot determine type of "total"  [has-type]` -- mypy REJECTS the
// module-level shape where it accepts the function-scope one -- while CPython
// prints 3 at exit 0. So the back-edge exemption that keeps the function-scope
// version clean must not reach module scope, which is automatic: that
// exemption requires the name to resolve in an ENCLOSING scope, and module
// scope has none. This is what makes NOT carrying loop_start_line onto a
// module-level placeholder correct rather than merely inert.
TEST(TypeChecker, AModuleLevelLoopCarriedAccumulatorStillReports) {
    const Checked checked = check_module("started = False\n"
                                         "for x in [1, 2, 3]:\n"
                                         "    if started:\n"
                                         "        print(total)\n"
                                         "    total = x\n"
                                         "    started = True\n"
                                         "print(total)\n");

    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "NameError");
}

// 2026-09-16, mypy PARTIAL NONE types. `x = None` does NOT declare `x` to be
// None: mypy records a partial type and takes the declared type from the next
// assignment that RESOLVES it. Every shape here was a false `TypeError:
// incompatible types in assignment (expression has type "int", variable has
// type "None")` on a program mypy calls Success and CPython RUNS -- measured
// 2026-09-16 against mypy 1.18.1 and CPython 3.14, 13 distinct shapes at
// module scope plus function, nested-function and class-body scope, where the
// rule is identical (scope does not change it). See Binding::partial_none.
TEST(TypeChecker, APartialNoneIsResolvedByTheNextAssignment) {
    // The headline shape, and the read-between and re-None variants.
    expect_clean("x = None\nx = 1\nprint(x)\n");
    expect_clean("x = None\nprint(x)\nx = 1\nprint(x)\n");
    // A `None` value leaves the binding partial rather than resolving it.
    expect_clean("x = None\nx = None\nx = 1\nprint(x)\n");
    // Never resolved at all: the declared type really is None, and this was
    // already correct before the fix -- kept as a control.
    expect_clean("x = None\nprint(x)\n");
    // A str resolver, so the rule is not read as numeric-tower specific.
    expect_clean("x = None\nx = \"s\"\nprint(x)\n");
    // Resolvers in every block form, and a `for` TARGET as the resolver.
    expect_clean("c: bool = True\nx = None\nif c:\n    x = 1\nprint(x)\n");
    expect_clean("c: bool = True\nif c:\n    x = None\nx = 1\nprint(x)\n");
    expect_clean("x = None\nfor i in [1]:\n    x = 1\nprint(x)\n");
    expect_clean("x = None\nfor x in [1, 2]:\n    print(x)\nprint(x)\n");
    // An already-Optional resolver.
    expect_clean("x = None\ny: int | None = 3\nx = y\nprint(x)\n");
    // Function, and class-body, scope.
    expect_clean("def f() -> None:\n    x = None\n    x = 1\n    print(x)\n\n\nf()\n");
    expect_clean("class C:\n    x = None\n    x = 1\n\n\nprint(C.x)\n");
}

// THE RESOLVED TYPE IS `T | None`, NOT `T`, and this test is the one that
// separates the two readings -- everything in the test above passes under
// either. reveal_type shows the NARROWED type (`x = None` / `x = 1` /
// `reveal_type(x)` is `builtins.int`), so an implementation built on it
// infers `T` and turns each of these from a correct report into a MISSED
// ERROR. Measured: mypy names the union in its own message text, and accepts
// a later `x = None`, and is byte-identically equivalent to writing
// `x: int | None = None`.
TEST(TypeChecker, APartialNoneResolvesToTheUnionWithNoneNotToTheBareType) {
    // mypy: Incompatible types in assignment (expression has type "str",
    // variable has type "int | None")  [assignment]
    const Checked str_after_int = check_module("x = None\nx = 1\nx = \"s\"\nprint(x)\n");
    const diagnostics::Diagnostic mismatch = only_error(str_after_int);
    EXPECT_EQ(mismatch.code, "TypeError");
    EXPECT_NE(mismatch.message.find("variable has type \"int | None\""), std::string::npos)
        << mismatch.message;

    // The resolver's type is taken EXACTLY, with no numeric-tower widening:
    // a `True` resolver gives `bool | None`, so a later `x = 1` is a genuine
    // error. mypy: variable has type "bool | None".
    const Checked int_after_bool = check_module("x = None\nx = True\nx = 1\nprint(x)\n");
    const diagnostics::Diagnostic exact = only_error(int_after_bool);
    EXPECT_EQ(exact.code, "TypeError");
    EXPECT_NE(exact.message.find("variable has type \"bool | None\""), std::string::npos)
        << exact.message;

    // THE SHARPEST PROBE: a module-level partial read from a `def` BELOW its
    // resolver. Nothing narrows across the scope boundary, so the DECLARED
    // type is what the return is checked against. mypy: Incompatible return
    // value type (got "int | None", expected "int"). Under the `T` reading
    // this program goes silent, because `int` satisfies `-> int`.
    const Checked returned =
        check_module("x = None\nx = 1\n\n\ndef f() -> int:\n    return x\n\n\nprint(f())\n");
    const diagnostics::Diagnostic returns = only_error(returned);
    EXPECT_EQ(returns.code, "TypeError");
    EXPECT_NE(returns.message.find("got \"int | None\""), std::string::npos) << returns.message;
}

// CONTROLS: the shapes that must KEEP reporting, each for its own reason.
TEST(TypeChecker, APartialNoneDoesNotSwallowGenuineErrors) {
    // An EXPLICIT `None` annotation defeats the partial entirely -- mypy:
    // Incompatible types in assignment (expression has type "int", variable
    // has type "None"). This is the one shape that was already correct.
    const Checked annotated = check_module("x: None = None\nx = 1\nprint(x)\n");
    EXPECT_EQ(only_error(annotated).code, "TypeError");

    // Two resolvers of differing type in the two arms of an if/else: the
    // first commits the declared type, the second is a real error. mypy:
    // variable has type "int | None".
    const Checked arms = check_module(
        "c: bool = True\nx = None\nif c:\n    x = 1\nelse:\n    x = \"s\"\nprint(x)\n");
    EXPECT_FALSE(arms.diagnostics.empty());

    // A method-frame attribute partial is DELIBERATELY not covered, and this
    // is a measured boundary, not an oversight: mypy confines a `self.x`
    // partial to the method frame that created it, so resolving it from
    // ANOTHER method is a real error -- `Incompatible types in assignment
    // (expression has type "int", variable has type "None")`, the identical
    // message and line this compiler already reports. It is also the
    // IDIOMATIC shape, so breaking it would be far worse than the
    // same-method sibling it leaves open (see CLAUDE.md).
    const Checked attribute =
        check_module("class C:\n    def __init__(self) -> None:\n        self.x = None\n\n"
                     "    def set(self) -> None:\n        self.x = 1\n\n\n"
                     "c = C()\nc.set()\nprint(c.x)\n");
    EXPECT_EQ(only_error(attribute).code, "TypeError");
}

// ADVERSARIAL REVIEW of the dict-store resolver, 2026-09-16. mypy's rule is
// "A PARTIAL CANNOT BE RESOLVED FROM A PARTIAL": a store whose KEY or VALUE is
// itself a partial-producing expression does not resolve, and mypy keeps
// reporting `Need type annotation for "x"` with reveal_type showing the
// UNRESOLVED `dict[Any, Any]`. Without the guard this compiler committed a
// concrete `dict[str, None]` mypy explicitly declines to infer, and went
// SILENT on all five shapes below.
//
// Two of them are also CPython `TypeError: unhashable type`, so the guard
// restores verdict agreement on programs BOTH oracles reject, not merely
// mypy's diagnostic.
TEST(TypeChecker, ADictStoreFromAPartialProducingExpressionIsNotAResolver) {
    for (const std::string source : {// A None VALUE, and a None KEY.
                                     std::string("x = {}\nx[\"a\"] = None\nprint(x)\n"),
                                     std::string("x = {}\nx[None] = 1\nprint(x)\n"),
                                     // A bare empty container in either position.
                                     std::string("x = {}\nx[\"a\"] = []\nprint(x)\n"),
                                     std::string("x = {}\nx[\"a\"] = {}\nprint(x)\n"),
                                     std::string("x = {}\nx[\"a\"] = set()\nprint(x)\n"),
                                     // CPython also rejects this one:
                                     // TypeError: unhashable type: 'list'.
                                     std::string("x = {}\nx[[]] = 1\nprint(x)\n")}) {
        const Checked checked = check_module(source);
        const diagnostics::Diagnostic error = only_error(checked);
        EXPECT_EQ(error.code, "TypeError") << source;
        EXPECT_NE(error.message.find("need type annotation"), std::string::npos)
            << source << " -> " << error.message;
    }
}

// CONTROLS for the guard above, and the reason it is consulted by the STORE
// branch ALONE. The asymmetry is mypy's: a DISPLAY wrapping a
// partial-producing expression is a COMPLETE type, so `x = []` / `x = [None]`
// is mypy Success -- only a BARE one standing alone blocks. A guard applied to
// the display resolver too would turn each of these into a false positive.
TEST(TypeChecker, ADisplayWrappingAPartialProducingExpressionStillResolves) {
    // The display sibling of the store shapes above: all mypy Success.
    expect_clean("x = []\nx = [None]\nprint(x)\n");
    expect_clean("x = {}\nx = {\"a\": None}\nprint(x)\n");
    // A store whose key/value are NON-bare containers still resolves.
    expect_clean("x = {}\nx[\"a\"] = [1]\nprint(x)\n");
    expect_clean("x = {}\nx[(1,)] = 1\nprint(x)\n");
}

// ADVERSARIAL REVIEW, 2026-09-16: the resolve typed the store's INDEX and then
// assign_subscript's own `type_of(target)` re-typed it, so every diagnostic the
// index raised was reported TWICE. Only reachable through the new store path --
// an annotated, a non-partial, and a LIST-partial receiver each reported once,
// which is what isolated the cause. assign_subscript now returns early on a
// successful resolve, which also makes the element-type check vacuous by
// construction (the resolve just committed V from this very value).
TEST(TypeChecker, ADictStoreIndexIsTypedExactlyOnce) {
    // only_error() fails outright if a second diagnostic is present, so this
    // asserts the count as much as the content.
    const Checked undefined = check_module("x = {}\nx[nope] = 1\nprint(x)\n");
    EXPECT_EQ(only_error(undefined).code, "NameError");

    // The three receivers that never reach the resolve path, each still
    // reporting exactly once.
    const Checked annotated =
        check_module("x: dict[str, int] = {}\nx[nope] = 1\nprint(x)\n");
    EXPECT_EQ(only_error(annotated).code, "NameError");

    const Checked non_partial = check_module("x = {\"k\": 1}\nx[nope] = 1\nprint(x)\n");
    EXPECT_EQ(only_error(non_partial).code, "NameError");
}

// 2026-09-16, THE `while c:` BINDER-NARROWING GAP -- the last HIGH-and-open
// entry in CLAUDE.md, open since 2026-09-12. `while c:` narrows `c` to
// `Literal[True]` for the loop body, so a guard on that same name is
// statically decided and a `break` in the branch the guard EXCLUDES is
// unreachable -- which makes the loop's `else` guaranteed to run, so the
// function always returns. Every shape here was `TypeError: missing return
// statement` from this compiler while `mypy --strict` said Success and
// CPython printed 1 then 3 at exit 0: both oracles accept AND RUN them.
//
// Measured 2026-09-16 across three parallel oracle sweeps. The recorded
// "five variants" figure was wrong in both directions -- SIX distinct
// break-reachability arrangements reproduce, and they fall out of one 2x2
// over {guard polarity} x {which arm holds the break}.
TEST(TypeChecker, AGuardNarrowedByItsOwnWhileConditionMakesABreakUnreachable) {
    // A1: guard true, break AFTER the if -- the if always leaves, so the
    // break is dead. The headline shape.
    expect_clean("def f(c: bool) -> int:\n    while c:\n        if c:\n            return 1\n"
                 "        break\n    else:\n        return 3\n\n\nprint(f(True))\n"
                 "print(f(False))\n");
    // A2: guard false, break INSIDE it -- the break's own arm is dead.
    expect_clean("def f(c: bool) -> int:\n    while c:\n        if not c:\n            break\n"
                 "        return 1\n    else:\n        return 3\n\n\nprint(f(True))\n"
                 "print(f(False))\n");
    // A3: guard true, break in the ELSE arm.
    expect_clean("def f(c: bool) -> int:\n    while c:\n        if c:\n            return 1\n"
                 "        else:\n            break\n    else:\n        return 3\n\n\n"
                 "print(f(True))\nprint(f(False))\n");
    // A4: guard false, break in the body, return in the else arm.
    expect_clean("def f(c: bool) -> int:\n    while c:\n        if not c:\n            break\n"
                 "        else:\n            return 1\n    else:\n        return 3\n\n\n"
                 "print(f(True))\nprint(f(False))\n");
    // A5: guard true with a `pass` body, break in the else arm, return after.
    expect_clean("def f(c: bool) -> int:\n    while c:\n        if c:\n            pass\n"
                 "        else:\n            break\n        return 1\n    else:\n"
                 "        return 3\n\n\nprint(f(True))\nprint(f(False))\n");
    // A6: an elif chain -- the dead first arm holds the break.
    expect_clean("def f(c: bool) -> int:\n    while c:\n        if not c:\n            break\n"
                 "        elif c:\n            return 1\n        else:\n            return 2\n"
                 "    else:\n        return 3\n\n\nprint(f(True))\nprint(f(False))\n");
    // The guard may sit deeper than the loop body's top level.
    expect_clean("def f(c: bool) -> int:\n    while c:\n        if c:\n            if c:\n"
                 "                return 1\n        break\n    else:\n        return 3\n\n\n"
                 "print(f(True))\nprint(f(False))\n");
}

// THE CONTROLS, and they matter more than the arm above: an implementation
// MORE aggressive than mypy's narrowing SILENCES a real `missing return
// statement`, while one LESS aggressive merely keeps a false positive. Every
// shape here is one mypy REPORTS, so this compiler must keep reporting it.
// Each was measured 2026-09-16; several RUN cleanly under CPython, so
// "CPython printed the expected output" is not evidence the narrowing held.
TEST(TypeChecker, WhileNarrowingDoesNotSilenceAReachableBreak) {
    // THE SINGLE MOST DANGEROUS SHAPE. The guard is statically TRUE, so its
    // own BODY stays live -- the break is REACHABLE and mypy reports. A fix
    // keyed on "a guard naming the loop condition kills the break" without
    // tracking POLARITY silences this. CPython genuinely returns None here.
    const Checked guard_true_break_inside =
        check_module("def f(c: bool) -> int:\n    while c:\n        if c:\n            break\n"
                     "    else:\n        return 3\n\n\nprint(f(True))\nprint(f(False))\n");
    EXPECT_EQ(only_error(guard_true_break_inside).code, "TypeError");

    // The mirror: guard statically FALSE kills the RETURN, and the break
    // after it survives.
    const Checked guard_false_kills_return =
        check_module("def f(c: bool) -> int:\n    while c:\n        if not c:\n"
                     "            return 1\n        break\n    else:\n        return 3\n\n\n"
                     "print(f(True))\nprint(f(False))\n");
    EXPECT_EQ(only_error(guard_false_kills_return).code, "TypeError");

    // The guard must name the LOOP CONDITION's own name. A guard on an
    // unrelated parameter decides nothing.
    const Checked unrelated_guard = check_module(
        "def f(c: bool, d: bool) -> int:\n    while c:\n        if d:\n            return 1\n"
        "        break\n    else:\n        return 3\n\n\nprint(f(True, True))\n"
        "print(f(True, False))\n");
    EXPECT_EQ(only_error(unrelated_guard).code, "TypeError");

    // AN UNGUARDED break is reachable. Pins that narrowing never makes the
    // whole body dead.
    const Checked unguarded =
        check_module("def f(c: bool) -> int:\n    while c:\n        break\n    else:\n"
                     "        return 3\n\n\nprint(f(True))\nprint(f(False))\n");
    EXPECT_EQ(only_error(unguarded).code, "TypeError");
}

// REBINDING KILLS THE NARROWING, whatever value is assigned -- and this is
// where a careless fix is most likely to silence a real error. Measured:
// `c = True` immediately before the guard makes mypy REPORT, because
// assigning a bare literal to a `bool`-declared name widens back to `bool`
// rather than re-narrowing. That shape PRINTS THE EXPECTED OUTPUT under
// CPython, so it looks correct and is not.
TEST(TypeChecker, ARebindingInTheLoopBodyKillsTheWhileNarrowing) {
    for (const std::string source :
         {// Rebound to True -- still a kill.
          std::string("def f(c: bool) -> int:\n    while c:\n        c = True\n        if c:\n"
                      "            return 1\n        break\n    else:\n        return 3\n\n\n"
                      "print(f(True))\nprint(f(False))\n"),
          // Rebound to False.
          std::string("def f(c: bool) -> int:\n    while c:\n        c = False\n        if c:\n"
                      "            return 1\n        break\n    else:\n        return 3\n\n\n"
                      "print(f(True))\nprint(f(False))\n"),
          // Rebound inside a nested `if` -- the reason this check must
          // RECURSE, and why it routes through for_each_own_scope_binding.
          std::string("def f(c: bool, d: bool) -> int:\n    while c:\n        if d:\n"
                      "            c = False\n        if c:\n            return 1\n"
                      "        break\n    else:\n        return 3\n\n\n"
                      "print(f(True, False))\nprint(f(False, False))\n"),
          // A TUPLE-UNPACK target. A scan that only matched a plain Name
          // target would miss it; this compiler has been bitten by exactly
          // that gap before.
          std::string("def f(c: bool) -> int:\n    while c:\n        c, x = False, 1\n"
                      "        if c:\n            return 1\n        break\n    else:\n"
                      "        return 3\n\n\nprint(f(True))\nprint(f(False))\n"),
          // Rebound inside an INNER loop before the guard.
          std::string("def f(c: bool, d: bool) -> int:\n    while c:\n        while d:\n"
                      "            c = False\n            d = False\n        if c:\n"
                      "            return 1\n        break\n    else:\n        return 3\n\n\n"
                      "print(f(True, True))\nprint(f(False, False))\n")}) {
        const Checked checked = check_module(source);
        const diagnostics::Diagnostic error = only_error(checked);
        EXPECT_EQ(error.code, "TypeError") << source;
        EXPECT_NE(error.message.find("missing return statement"), std::string::npos)
            << source << " -> " << error.message;
    }
}

// THE DECLARED TYPE IS SAFETY-CRITICAL, not fussiness. Truthiness narrowing
// yields a statically DECIDABLE literal only for `bool` (and `bool | None`,
// omitted for scope). Measured 2026-09-16: mypy REPORTS for every type below,
// because `while c:` narrows `c: int` to `int` and `c: object` to `object` --
// neither of which decides the guard. Narrowing without this check would
// silence all four.
TEST(TypeChecker, WhileNarrowingAppliesOnlyToABoolCondition) {
    for (const std::string annotation : {std::string("int"), std::string("str"),
                                         std::string("float"), std::string("object")}) {
        const Checked checked =
            check_module("def f(c: " + annotation +
                         ") -> int:\n    while c:\n        if c:\n            return 1\n"
                         "        break\n    else:\n        return 3\n");
        const diagnostics::Diagnostic error = only_error(checked);
        EXPECT_EQ(error.code, "TypeError") << annotation;
        EXPECT_NE(error.message.find("missing return statement"), std::string::npos)
            << annotation << " -> " << error.message;
    }
}

// 2026-09-17, LITERAL CONDITION FOLDING. mypy prunes a statically-decided
// branch before asking any reachability question; these helpers had no
// constant folding at all, which produced false positives in FOUR separate
// places on programs both oracles accept and run. mypy's own authority is
// `is_true_literal`/`is_false_literal` at the top of
// `find_isinstance_check_helper`, mypy 1.18.1 `checker.py:8255`.
//
// F1 -- a folded guard did not decide always_returns, because that arm
// requires a NON-EMPTY `orelse`. This is the most ordinary shape in the whole
// family: no loop, no `break`, no dead code. Every case below was
// `TypeError: missing return statement` here while `mypy --strict` said
// Success and CPython printed the value at exit 0.
TEST(TypeChecker, ALiteralTrueGuardDecidesTheReturnPath) {
    // P1: the entire function body.
    expect_clean("def f() -> int:\n    if True:\n        return 1\n\n\nprint(f())\n");
    // P2: with a trailing statement after the decided `if`.
    expect_clean("def f() -> int:\n    if True:\n        return 1\n    print(\"after\")\n\n\n"
                 "print(f())\n");
    // P3: an explicit, non-returning `else` -- the pre-existing both-arms rule
    // answers false for this one, so the fold is the sole cause.
    expect_clean("def f() -> int:\n    if True:\n        return 1\n    else:\n        pass\n\n\n"
                 "print(f())\n");
    // P4: the decided guard is an `elif`, i.e. nested in the outer `orelse`.
    expect_clean("def f(c: bool) -> int:\n    if c:\n        return 1\n    elif True:\n"
                 "        return 2\n\n\nprint(f(True))\nprint(f(False))\n");
    // P5: a bare nonzero int, which mypy folds by VALUE exactly as it folds
    // `True` -- `is_true_literal` is `refers_to_fullname(..., "builtins.True")
    // or isinstance(n, IntExpr) and n.value != 0`.
    expect_clean("def f() -> int:\n    if 1:\n        return 1\n\n\nprint(f())\n");
}

// F2 -- a folded-true `return` did not start an unreachable REGION, so the
// statement after it was still type-checked. A DIFFERENT DIAGNOSTIC CLASS from
// the rest of this family: a false `incompatible types in assignment`, not a
// missing-return. It needed no change to check_suite, which already consumes
// statement_always_leaves -- putting the fold inside that predicate (rather
// than threading it as a parameter the way binder narrowing is threaded) is
// what makes this family fall out for free.
TEST(TypeChecker, ALiteralTrueGuardStartsAnUnreachableRegion) {
    // P6. mypy Success (confirmed under --warn-unreachable as
    // `Statement is unreachable`), CPython prints 1 at exit 0.
    expect_clean("def f() -> int:\n    if True:\n        return 1\n    x: int = \"s\"\n"
                 "    return 2\n\n\nprint(f())\n");
    // P7: the same shape one suite deeper, inside a `while` body.
    expect_clean("def f(c: bool) -> int:\n    while c:\n        if True:\n            return 1\n"
                 "        x: int = \"s\"\n    else:\n        return 3\n\n\nprint(f(True))\n"
                 "print(f(False))\n");
}

// F3 -- an always-true loop condition that is not spelled `True`. The deleted
// `is_literal_true` matched a BOOL_TRUE Constant alone, so `while 1:` and
// `while 2:` were false `missing return statement`s even though mypy treats
// them as never-exiting exactly as it treats `while True:`. That helper was a
// second, independent notion of "literally true" and is gone; both call sites
// now ask literal_guard_verdict.
TEST(TypeChecker, AnAlwaysTrueLoopConditionIsNotOnlyTheWordTrue) {
    // P8/P9: type-only -- these hang by design under CPython, so no driver.
    expect_clean("def f() -> int:\n    while 1:\n        pass\n");
    expect_clean("def f() -> int:\n    while 2:\n        pass\n");
    // P10: the gap type_checker.h's own second bullet recorded as open since
    // 2026-09-12 -- `while True:` whose only `break` sits under a folded-FALSE
    // guard, so the loop never exits. CPython prints 1 at exit 0.
    expect_clean("def f() -> int:\n    while True:\n        if False:\n            break\n"
                 "        return 1\n\n\nprint(f())\n");
}

// F4 -- a folded guard did not kill a `break`. This is the family a8cae40's
// binder narrowing already handled for a NAMED condition; folding reaches the
// same decided-guard path through guard_verdict. The `for` sibling is the
// sharpest case: a `for` loop never produces a narrowed name, so
// `narrowed_true_name == nullptr` forced Unknown and a8cae40's entire
// decided-guard path -- its always_leaves_branch stop included -- was DEAD
// CODE for every `for` loop.
TEST(TypeChecker, ALiteralGuardDecidesBreakReachability) {
    // P12, the headline `while` shape. CPython prints 1 then 3.
    expect_clean("def f(c: bool) -> int:\n    while c:\n        if True:\n            return 1\n"
                 "        break\n    else:\n        return 3\n\n\nprint(f(True))\n"
                 "print(f(False))\n");
    // P14, the `for` sibling. CPython prints 7 then 3.
    expect_clean("def f(xs: list[int]) -> int:\n    for x in xs:\n        if True:\n"
                 "            return x\n        break\n    else:\n        return 3\n\n\n"
                 "print(f([7, 8]))\nprint(f([]))\n");
    // P16, the guard nested two and three decided guards deep -- the shape
    // a8cae40's own first implementation missed for narrowing, reached here
    // through the same recursive thread.
    expect_clean("def f(c: bool) -> int:\n    while c:\n        if True:\n            if True:\n"
                 "                return 1\n        break\n    else:\n        return 3\n\n\n"
                 "print(f(True))\nprint(f(False))\n");
    expect_clean("def f(c: bool) -> int:\n    while c:\n        if True:\n            if True:\n"
                 "                if True:\n                    return 1\n        break\n"
                 "    else:\n        return 3\n\n\nprint(f(True))\nprint(f(False))\n");
    // P17: an `elif` chain whose dead arm holds the break.
    expect_clean("def f(c: bool) -> int:\n    while c:\n        if True:\n            return 1\n"
                 "        elif c:\n            break\n    else:\n        return 3\n\n\n"
                 "print(f(True))\nprint(f(False))\n");
}

// P13, AND THE DIVERGENCE FROM a8cae40 THAT IS EASIEST TO GET WRONG BY
// ANALOGY. loop_narrows_truthy requires the condition's declared type to be
// exactly `bool`, because narrowing a non-bool decides nothing and seven
// measured types make mypy REPORT. Literal folding is the exact INVERSE:
// measured 2026-09-17, the P12 shape is mypy-Success and CPython-clean with
// `c` declared `bool`, `int`, `str`, `float`, `list[int]` AND `object`, all
// six -- the guard is decided by its own literal, so the condition's type is
// irrelevant. Routing folding through that bool gate would retain five of
// these six false positives.
TEST(TypeChecker, LiteralFoldingIgnoresTheLoopConditionsDeclaredType) {
    for (const char* const head :
         {"def f(c: int) -> int:\n", "def f(c: str) -> int:\n", "def f(c: float) -> int:\n",
          "def f(c: list[int]) -> int:\n", "def f(c: object) -> int:\n"}) {
        expect_clean(std::string(head) +
                     "    while c:\n        if True:\n            return 1\n        break\n"
                     "    else:\n        return 3\n");
    }
}

// THE SAFETY CONTROLS, and they matter more than every accepting test above:
// a fold MORE aggressive than mypy's SILENCES a real `missing return
// statement`, while one LESS aggressive merely keeps a false positive. Every
// shape here is one mypy REPORTS, so this compiler must keep reporting it.
//
// All are mypy-ONLY rejections -- CPython exits 0 for each, because Python
// performs no return-type check at runtime. The observable harm is a None
// leaking out of an `-> int` function, and after codegen a C++ slot default.
// That does not weaken them: the union rule says a program either oracle
// rejects must not be silently accepted.
//
// POLARITY DECIDES WHICH ARM DIES, never that the whole `if` is dead. An
// always-TRUE guard leaves its own BODY live.
TEST(TypeChecker, AFoldedTrueGuardLeavesItsOwnBodyLive) {
    // C1. THE SINGLE MOST DANGEROUS SHAPE, and the sharpest pair in this work
    // when set against `if not True: break` -- same literal, same `break`,
    // opposite verdicts from mypy. A fix keyed on "a literal guard kills the
    // break it contains" flips this to silent.
    const Checked while_break =
        check_module("def f(c: bool) -> int:\n    while c:\n        if True:\n            break\n"
                     "    else:\n        return 3\n");
    EXPECT_EQ(only_error(while_break).code, "TypeError");

    // C2, the `for` sibling of C1 -- and the one the fold newly EXPOSES,
    // since this decided-guard path was unreachable for `for` loops before.
    const Checked for_break =
        check_module("def f(xs: list[int]) -> int:\n    for x in xs:\n        if True:\n"
                     "            break\n    else:\n        return 3\n");
    EXPECT_EQ(only_error(for_break).code, "TypeError");

    // C4: the live arm leaves via `break`, but the loop has no `else`, so the
    // function can still fall off its end.
    const Checked no_loop_else = check_module(
        "def f(c: bool) -> int:\n    while c:\n        if True:\n            return 1\n"
        "        else:\n            break\n");
    EXPECT_EQ(only_error(no_loop_else).code, "TypeError");

    // C6: a folded-TRUE guard whose body does NOT return decides nothing --
    // the fold answers WHICH arm is live, never that the `if` leaves.
    const Checked true_pass = check_module("def f() -> int:\n    if True:\n        pass\n");
    EXPECT_EQ(only_error(true_pass).code, "TypeError");

    // C11: `while True:` with a genuinely LIVE guarded break.
    const Checked live_break =
        check_module("def f(c: bool) -> int:\n    while True:\n        if c:\n            break\n");
    EXPECT_EQ(only_error(live_break).code, "TypeError");

    // C12: the decided guard sits in an INNER loop, so it cannot kill the
    // OUTER loop's own break.
    const Checked outer_break =
        check_module("def f(c: bool, xs: list[int]) -> int:\n    while c:\n        for x in xs:\n"
                     "            if True:\n                return x\n        break\n"
                     "    else:\n        return 3\n");
    EXPECT_EQ(only_error(outer_break).code, "TypeError");
}

// THE POLARITY CONTROL PROPER: a folded-FALSE guard's own BODY is DEAD, so
// whatever leaves in there must NOT count. Treating AlwaysFalse as AlwaysTrue
// silences every shape below -- that is the signature of a change which
// silences a real error while looking like it closed a false positive.
TEST(TypeChecker, AFoldedFalseGuardsDeadBodyDoesNotDecideAnything) {
    // C7/C8: `if False:` / `if 0:` guarding the only return. mypy reports.
    const Checked false_return = check_module("def f() -> int:\n    if False:\n        return 1\n");
    EXPECT_EQ(only_error(false_return).code, "TypeError");
    const Checked zero_return = check_module("def f() -> int:\n    if 0:\n        return 1\n");
    EXPECT_EQ(only_error(zero_return).code, "TypeError");
    // `None` folds FALSE too, and is the third member of the falsy set this
    // project records (re-measured 2026-09-17: `()` is ALSO pruned by mypy and
    // `0.0` is NOT, so the recorded `{False, 0, None}` set is incomplete
    // rather than wrong).
    const Checked none_return = check_module("def f() -> int:\n    if None:\n        return 1\n");
    EXPECT_EQ(only_error(none_return).code, "TypeError");

    // The dead body holds the return, so the `break` after it stays LIVE and
    // the loop's `else` is not guaranteed. This is the polarity mirror of C1
    // and the shape a folded-FALSE-as-TRUE bug makes silent.
    const Checked dead_return_live_break = check_module(
        "def f(c: bool) -> int:\n    while c:\n        if False:\n            return 1\n"
        "        break\n    else:\n        return 3\n");
    EXPECT_EQ(only_error(dead_return_live_break).code, "TypeError");

    // A folded-FALSE LOOP condition means the loop never RUNS, which is the
    // opposite of never exiting -- so the While arms test `== AlwaysTrue` and
    // not `!= Unknown`.
    const Checked while_zero = check_module("def f() -> int:\n    while 0:\n        pass\n");
    EXPECT_EQ(only_error(while_zero).code, "TypeError");
    const Checked while_none = check_module("def f() -> int:\n    while None:\n        pass\n");
    EXPECT_EQ(only_error(while_none).code, "TypeError");
}

// THE GATE IS THE AST SHAPE, NEVER THE TYPE -- a bare `ast::Constant` and
// nothing else, the same discipline emit_power uses for `**`. A signed literal
// parses as `UnaryOp(-, Constant)`, so the sign lives in a node the TYPE
// cannot see and `-1` types as `int` exactly as `1` does. This is mypy's own
// exclusion, structurally: `-1` is a `UnaryExpr` and never an `IntExpr`, so it
// never reaches `is_true_literal` -- which is why mypy folds `(1)` but not
// `(-1)`. Measured 2026-09-17: mypy reports for all three below.
TEST(TypeChecker, ASignedLiteralGuardIsNotFolded) {
    for (const char* const literal : {"-1", "+1", "-0"}) {
        const Checked checked =
            check_module(std::string("def f() -> int:\n    if ") + literal +
                         ":\n        return 1\n");
        EXPECT_EQ(only_error(checked).code, "TypeError") << literal;
    }
}

// INT LEXEMES ARE WHITELISTED AS DECIMAL, NEVER BLACKLISTED, and this is the
// one place where the cheap implementation is wrong in the UNACCEPTABLE
// direction. `0x0` arrives here as a LITERAL_INT whose lexeme is "0x0"
// (verified via --tokens), so the obvious "is the lexeme all zeros" test
// answers false and would fold it TRUE -- killing the break below, guaranteeing
// the `else`, and making cythonpp SILENT on a program mypy REJECTS.
//
// The test is per-CHARACTER and not a check for the prefix `0x`: the
// UPPER-CASE spellings are as much non-decimal as the lower-case ones, and all
// six of 0x0/0X0/0o0/0O0/0b0/0B0 were measured to fold FALSE under mypy.
TEST(TypeChecker, ANonDecimalIntLexemeIsNotFolded) {
    // The safety-critical half: every one of these folds FALSE under mypy, so
    // the break stays live and this compiler must keep reporting.
    for (const char* const literal : {"0x0", "0X0", "0o0", "0O0", "0b0", "0B0"}) {
        const Checked checked = check_module(
            std::string("def f(c: bool) -> int:\n    while c:\n        if ") + literal +
            ":\n            return 1\n        break\n    else:\n        return 3\n");
        EXPECT_EQ(only_error(checked).code, "TypeError") << literal;
    }
    // The cost, pinned deliberately: a non-decimal NONZERO literal folds TRUE
    // under mypy (`0x1`/`0X1`/`0b1`/`0o1` all measured Success) and this
    // compiler retains the false positive rather than reading the value.
    // Omitting a form only ever RETAINS a false positive, the safe direction,
    // and handling every base is a purely additive widening later.
    const Checked hex_one = check_module("def f() -> int:\n    if 0x1:\n        return 1\n");
    EXPECT_EQ(only_error(hex_one).code, "TypeError");
}

// A LEADING ZERO followed by a nonzero digit is not a legal Python decimal
// literal at all, and the whitelist must not fold it TRUE. Measured
// 2026-09-17: `if 0_1:` is a mypy BLOCKING `Leading zeros in decimal integer
// literals are not permitted [syntax]` and a CPython `SyntaxError`, so BOTH
// oracles reject the program. This compiler has no diagnostic of its own for
// the literal (a separate, pre-existing parser gap) and today exits non-zero
// only because of the very missing-return this work removes -- so folding it
// TRUE would turn a right-verdict/wrong-message rejection into SILENT
// acceptance of a program both oracles reject.
TEST(TypeChecker, ALeadingZeroIntLexemeIsNotFolded) {
    const Checked checked = check_module("def f() -> int:\n    if 0_1:\n        return 1\n");
    EXPECT_EQ(only_error(checked).code, "TypeError");

    // The all-zero leading-zero spellings ARE legal Python and DO fold false,
    // so they must keep reporting for the ordinary reason rather than being
    // excluded by the clause above.
    for (const char* const literal : {"00", "000", "0_0"}) {
        const Checked zero = check_module(std::string("def f() -> int:\n    if ") + literal +
                                          ":\n        return 1\n");
        EXPECT_EQ(only_error(zero).code, "TypeError") << literal;
    }
}

// Forms mypy does NOT fold at all, so including any of them would make
// cythonpp accept a program mypy rejects. Measured 2026-09-17 in three
// independent templates. Note `0.0` is here and NOT in the falsy fold set --
// the set is not "numeric zero".
TEST(TypeChecker, AnUnfoldableGuardOrLoopConditionStillReports) {
    // C5: nothing foldable at all -- the pre-existing behaviour.
    const Checked plain = check_module("def f(c: bool) -> int:\n    if c:\n        return 1\n");
    EXPECT_EQ(only_error(plain).code, "TypeError");
    // C10, plus the float and empty-string forms.
    for (const char* const literal : {"\"x\"", "\"\"", "0.0", "1.0"}) {
        const Checked checked =
            check_module(std::string("def f() -> int:\n    if ") + literal +
                         ":\n        return 1\n");
        EXPECT_EQ(only_error(checked).code, "TypeError") << literal;
    }
    // C13: an unfoldable LOOP condition must not be read as never-exiting.
    for (const char* const condition : {"\"x\"", "[1]", "1.0"}) {
        const Checked checked = check_module(std::string("def f() -> int:\n    while ") +
                                             condition + ":\n        pass\n");
        EXPECT_EQ(only_error(checked).code, "TypeError") << condition;
    }
}

// TWO mypy-UNSOUND FORMS, EXCLUDED BY CONSTRUCTION rather than by a check, and
// pinned so a future widening cannot quietly admit them. Measured 2026-09-17:
// mypy prunes `NotImplemented` as always-true while CPython raises
// `TypeError: NotImplemented should not be used in a boolean context` at exit
// 1, and mypy prunes `not TYPE_CHECKING` while the pruned branch actually
// RUNS, returning None from an `-> int` function. Following mypy on either
// would compile a program CPython refuses to run.
//
// Both are bare `ast::Name`s, never an `ast::Constant`, so
// literal_guard_verdict cannot reach either -- which is ALSO why the
// diagnostic below is a NameError rather than a missing-return: these names
// are in no builtin table this compiler models (a separate, recorded false
// positive of its own, unrelated to folding). The assertion that matters is
// that neither is silently accepted.
TEST(TypeChecker, AMypyUnsoundNameGuardIsNeverFolded) {
    const Checked not_implemented =
        check_module("def f() -> int:\n    if NotImplemented:\n        return 1\n");
    EXPECT_FALSE(not_implemented.diagnostics.empty());
    const Checked type_checking =
        check_module("def f() -> int:\n    if not TYPE_CHECKING:\n        pass\n"
                     "    else:\n        return 1\n");
    EXPECT_FALSE(type_checking.diagnostics.empty());
}

// RETAINED FALSE POSITIVES, pinned because the spec's own measurement table
// lists them as shapes to close and the IMPLEMENTED fold set cannot: every one
// is a `not` over a folded literal, and `not`/`and`/`or` are deliberately
// excluded. mypy's behaviour there is inconsistent with its own atom rules
// (`not True` folds false, and `"" or 1` folds TRUE even though `""` alone
// does not fold at all), and `and` is ONE-sided where `or` is TWO-sided, so
// modelling them needs its own sweep. All three below are mypy-Success and
// CPython-clean, and this compiler still reports -- a retained false positive,
// which is the safe direction, not a violation.
TEST(TypeChecker, ANotOverAFoldedLiteralIsDeliberatelyNotFolded) {
    const Checked while_not_false =
        check_module("def f() -> int:\n    while not False:\n        pass\n");
    EXPECT_EQ(only_error(while_not_false).code, "TypeError");
    const Checked break_under_not_true =
        check_module("def f() -> int:\n    while True:\n        if not True:\n            break\n"
                     "        return 1\n");
    EXPECT_EQ(only_error(break_under_not_true).code, "TypeError");
    const Checked guard_else_under_not_true =
        check_module("def f(c: bool) -> int:\n    while c:\n        if not True:\n"
                     "            break\n        else:\n            return 1\n    else:\n"
                     "        return 3\n");
    EXPECT_EQ(only_error(guard_else_under_not_true).code, "TypeError");
}

} // namespace
} // namespace cythonpp::domain::semantic
