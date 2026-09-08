#include <memory>
#include <string>
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

} // namespace
} // namespace cythonpp::domain::semantic
