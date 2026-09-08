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

TEST(TypeChecker, ADefAndAClassSharingANameIsARedefinition) {
    const Checked checked = check_module("def n() -> None:\n    pass\nclass n:\n    pass\n");

    EXPECT_EQ(only_error(checked).code, "TypeError");
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

// Verified: a function body sees a module global defined AFTER the def.
TEST(TypeChecker, AFunctionBodySeesGlobalsDefinedBelowIt) {
    expect_clean("def f() -> int:\n    return g\ng: int = 5\n");
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

} // namespace
} // namespace cythonpp::domain::semantic
