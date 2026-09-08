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

// Fix round 1, Finding 3: pin the MESSAGE, not just the code. The entire
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

// Fix round 1, Finding 2: an AnnAssign followed by a colliding `def` was
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

// Fix round 1, Finding 5: ReportsABareEmptyContainer above only pins `[]`.
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

// Fix round 1, Finding 1: this test was VACUOUS before this fix round --
// `return g` walks to RecursiveVisitor::visit(Return), which visits the Name
// `g` via accept(), and TypeChecker's Name arm is RecursiveVisitor's own
// no-op default (Name carries nothing to check), so `g` was never typed at
// all and the test could not have failed regardless of what the checker did.
// Swapped for `print(g)`, an ExprStmt TypeChecker DOES override, so `g`
// actually reaches ExpressionTyper::type_of_name. This also needed
// TypeChecker::visit(FunctionDef) to push a Function scope (see type_checker.h/
// .cpp) -- without it, the body was checked in the still-current Module
// scope, so this read resolved in_own_scope == true and falsely reported
// "used before definition" against `g`'s later module-level binding. Both
// forms verified mypy-clean.
TEST(TypeChecker, AFunctionBodySeesGlobalsDefinedBelowIt) {
    expect_clean("def f() -> int:\n    print(g)\n    return g\ng: int = 5\n");
    expect_clean("def f() -> None:\n    print(x)\nx = 5\n");
}

// Fix round 1, Finding 6: the in_own_scope gate itself (ExpressionTyper's
// Name arm) and statement_line_'s INT_MAX default had zero DIRECT coverage
// before Finding 1 added a Function scope to check against -- every existing
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

TEST(TypeChecker, ACompatibleDefaultIsClean) {
    expect_clean("def f(x: int = 5) -> None:\n    pass\n");
}

// Verified clean: module-level names are visible ahead of their definition
// inside function bodies, and the function's own name is one of them.
//
// PLAN DEFECT (reported, not silently patched): the brief's own version of
// this test used `return f(n)`, which is VACUOUS -- Return is not yet
// overridden (Task 20) and RecursiveVisitor's default just walks to the
// Name "f", whose own visit() is RecursiveVisitor's no-op default, so
// nothing is ever typed through ExpressionTyper and the test would pass
// regardless of whether recursion resolution works at all. This is the
// exact same class of defect Task 17's fix round 1 found and fixed for
// `return g` (rewritten to `print(g)`). Rewritten the same way here, inside
// the `if`, so the call genuinely reaches ExpressionTyper via the
// TypeChecker-overridden ExprStmt arm.
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
// PLAN DEFECT (reported, not silently patched): the brief's own version used
// `return v` inside `i`, which is the SAME vacuous-Return shape as
// DirectRecursionIsClean above -- `v` is never typed through ExpressionTyper
// either way, so the test cannot fail regardless of the implementation.
// Rewritten to `print(v)`, matching the same established fix pattern.
TEST(TypeChecker, AClosureMayReadALocalAssignedAfterItsOwnDef) {
    expect_clean(
        "def o() -> int:\n"
        "    def i() -> int:\n"
        "        print(v)\n"
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

// ---------------------------------------------------------------------------
// Task 18, fix round 1.
// ---------------------------------------------------------------------------

// Finding 1 (CRITICAL). Reproduced against the built binary before this fix:
// `def f(x: int) -> None: print(x)` reported a false "used before
// definition" on `x`, because a one-line suite's body statement sits on the
// SAME line as the `def` -- exactly the line a parameter is bound at -- so
// the ordinary ordering check's `>=` misfired. mypy accepts both the
// one-line and two-line forms; this pins the one-line form, the two-line
// form already being covered by AFullyAnnotatedFunctionIsClean.
TEST(TypeChecker, AOneLineDefReadingItsOwnParameterIsClean) {
    expect_clean("def f(x: int) -> None: print(x)\n");
}

// Finding 1, second symptom: the SAME root cause, in the opposite direction.
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

// Finding 2 (IMPORTANT). Pins top_level_signatures_'s entire reason to
// exist: without the cache, visit(FunctionDef) would call AnnotationResolver
// a SECOND time on the same bad annotation collect_signatures's Phase 2
// already resolved once, double-reporting it. Deleting the cache (and
// falling through to the `else` branch unconditionally) makes this test
// fail with 2 diagnostics instead of 1.
TEST(TypeChecker, ATopLevelDefsBadAnnotationIsReportedExactlyOnce) {
    const Checked checked = check_module("def f(x: Bogus) -> None:\n    pass\n");

    EXPECT_EQ(only_error(checked).code, "NameError");
}

// Finding 3 (IMPORTANT). ANestedDefIsNotHoisted only pins the NEGATIVE case
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

// Finding 4 (Minor). collect_signatures already checked ScopeStack::bind's
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

// Finding 5 (Minor). The zero-parameter-method path reported its own
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

// Verified: the class body is NOT in the method's lexical scope.
//
// PLAN DEFECT (reported, not silently patched): the brief's own version of
// this test used `return x`, which is VACUOUS for the exact reason recorded
// on DirectRecursionIsClean and AClosureMayReadALocalAssignedAfterItsOwnDef
// above -- Return is not yet overridden (Task 20), so RecursiveVisitor's
// default just walks to the Name `x` via accept(), whose own visit() is
// RecursiveVisitor's no-op default; `x` never reaches ExpressionTyper and the
// test could not fail regardless of whether a method body actually skips the
// class scope. Rewritten to `print(x)`, the same established fix.
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

// Task 19 gap 2 (recorded as pre-existing, closed by this task): collect_
// classes used to declare EVERY top-level ClassDef, even one scan_top_level_
// names had already reported as a losing same-name collision -- silently
// overwriting the winning class's ClassTable entry, so a later member lookup
// resolved against the wrong (erroneous) class. The winning class's own
// member must survive the collision.
TEST(TypeChecker, ALosingClassRedefinitionDoesNotClobberTheWinningOnesMembers) {
    const Checked checked = check_module(
        "class C:\n"
        "    x: int\n"
        "class C:\n"
        "    y: str\n"
        "c = C()\n"
        "z: int = c.x\n");

    const diagnostics::Diagnostic error = only_error(checked);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "name \"C\" already defined on line 1");
}

// Task 18 fix round 2 (the fourth call site the round-1 review missed).
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

} // namespace
} // namespace cythonpp::domain::semantic
