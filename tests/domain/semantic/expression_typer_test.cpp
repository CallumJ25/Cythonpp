#include <map>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "domain/ast/expr_stmt.h"
#include "domain/ast/module.h"
#include "domain/diagnostics/diagnostic_sink.h"
#include "domain/lexer/indentation_pass.h"
#include "domain/lexer/lexer.h"
#include "domain/parser/statement_parser.h"
#include "domain/semantic/class_table.h"
#include "domain/semantic/expression_typer.h"
#include "domain/semantic/scope_stack.h"
#include "domain/semantic/type_map.h"
#include "domain/semantic/type_name.h"

namespace cythonpp::domain::semantic {
namespace {

struct Typed {
    Type type;
    std::string printed;
    std::vector<diagnostics::Diagnostic> diagnostics;
    std::size_t map_size = 0;
};

// One expression through the REAL chain: source -> Lexer -> IndentationPass
// -> StatementParser -> ExprStmt::value() -> ExpressionTyper. Hand-building
// the Expr would let these tests agree with a wrong belief about what the
// parser produces.
Typed type_expression(const std::string& expression,
                      const std::map<std::string, Type>& bindings = {},
                      const Type& expected = Type::unknown(),
                      const ClassTable* table = nullptr) {
    const std::string source = expression + "\n";
    lexer::Lexer lexer(source);
    const lexer::TokenStream lexed(lexer.tokenize());

    // A SEPARATE sink for the parse, asserted empty, so a fixture that fails
    // to parse fails loudly rather than silently testing nothing.
    diagnostics::DiagnosticSink parse_sink;
    lexer::TokenStream tokens = lexer::IndentationPass().run(lexed, parse_sink);
    const std::unique_ptr<ast::Module> module =
        parser::StatementParser(tokens, parse_sink).parse_module();
    EXPECT_TRUE(parse_sink.empty()) << "fixture must parse cleanly: " << expression;
    EXPECT_EQ(module->body().size(), 1u) << "fixture must be one statement";

    const auto* statement = dynamic_cast<const ast::ExprStmt*>(module->body().front().get());
    EXPECT_NE(statement, nullptr) << "fixture must be an expression statement";

    ScopeStack scopes;
    for (const auto& entry : bindings) {
        Binding binding;
        binding.type = entry.second;
        binding.declared_line = 1;
        scopes.bind(entry.first, binding);
    }

    ClassTable owned;
    const ClassTable& classes = table != nullptr ? *table : owned;

    TypeMap types;
    diagnostics::DiagnosticSink sink;
    Typed result;
    result.type = ExpressionTyper(scopes, classes, types, sink).type_of(statement->value(),
                                                                       expected);
    result.printed = type_name(result.type);
    result.diagnostics = sink.diagnostics();
    result.map_size = types.size();
    return result;
}

// Asserts clean, so a test reading only the type still fails loudly if the
// typer reported on the way to the right answer.
std::string typed_name(const std::string& expression,
                       const std::map<std::string, Type>& bindings = {}) {
    const Typed typed = type_expression(expression, bindings);
    EXPECT_TRUE(typed.diagnostics.empty())
        << "expected no diagnostics for " << expression << ", got "
        << (typed.diagnostics.empty() ? "" : typed.diagnostics.front().message);
    return typed.printed;
}

// Asserts the COUNT, so a rule that reports twice cannot hide behind a
// message match. The only thing that pins one diagnostic per root cause.
diagnostics::Diagnostic only_error(const Typed& typed) {
    EXPECT_EQ(typed.diagnostics.size(), 1u) << "expected exactly one diagnostic";
    if (typed.diagnostics.size() != 1) {
        return diagnostics::Diagnostic{diagnostics::Severity::Error, "", "", 0, 0};
    }
    return typed.diagnostics.front();
}

TEST(ExpressionTyper, TypesEveryLiteralKind) {
    EXPECT_EQ(typed_name("1"), "int");
    EXPECT_EQ(typed_name("1.5"), "float");
    EXPECT_EQ(typed_name("1j"), "complex");
    EXPECT_EQ(typed_name("\"s\""), "str");
    EXPECT_EQ(typed_name("b\"s\""), "bytes");
    EXPECT_EQ(typed_name("True"), "bool");
    EXPECT_EQ(typed_name("False"), "bool");
    EXPECT_EQ(typed_name("None"), "None");
    EXPECT_EQ(typed_name("..."), "ellipsis");
}

TEST(ExpressionTyper, ResolvesABoundName) {
    EXPECT_EQ(typed_name("x", {{"x", Type::str()}}), "str");
}

TEST(ExpressionTyper, ReportsAnUnboundName) {
    const Typed typed = type_expression("nope");

    const diagnostics::Diagnostic error = only_error(typed);
    EXPECT_EQ(error.code, "NameError");
    EXPECT_EQ(error.message, "name 'nope' is not defined");
    EXPECT_EQ(error.line, 1);
    EXPECT_EQ(error.column, 1);
    EXPECT_EQ(typed.printed, "Unknown") << "a failed lookup must yield Unknown";
}

TEST(ExpressionTyper, TypesArithmeticThroughTheRuleTable) {
    EXPECT_EQ(typed_name("1 + 2"), "int");
    EXPECT_EQ(typed_name("1 / 2"), "float") << "Python 3 true division";
    EXPECT_EQ(typed_name("1 // 2"), "int");
    EXPECT_EQ(typed_name("\"a\" + \"b\""), "str");
    EXPECT_EQ(typed_name("\"a\" * 3"), "str");
}

TEST(ExpressionTyper, ReportsAGenuineOperandTypeError) {
    const Typed typed = type_expression("1 + \"s\"");

    const diagnostics::Diagnostic error = only_error(typed);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "unsupported operand types for + (\"int\" and \"str\")");
}

// The three-valued result's whole point: a modelling limit must be
// NotImplementedError, never TypeError, because the program may be one mypy
// accepts. Verified: `v + 1` with __add__ defined is mypy-clean.
TEST(ExpressionTyper, ReportsAUserClassOperatorAsUnsupportedNotAsATypeError) {
    ClassTable table;
    table.declare("Widget", {});
    const Typed typed =
        type_expression("w + 1", {{"w", Type::class_of("Widget")}}, Type::unknown(), &table);

    const diagnostics::Diagnostic error = only_error(typed);
    EXPECT_EQ(error.code, "NotImplementedError");
    EXPECT_EQ(error.message, "operators on user-defined class instances are not supported");
}

TEST(ExpressionTyper, ReportsAUnionOperandAsUnsupported) {
    const Typed typed = type_expression(
        "x + 1", {{"x", Type::union_of({Type::int_(), Type::none()})}});

    const diagnostics::Diagnostic error = only_error(typed);
    EXPECT_EQ(error.code, "NotImplementedError");
    EXPECT_EQ(error.message,
              "operations on a union-typed value require narrowing, which is not supported");
}

// Unknown is absorbing, so one root cause draws ONE diagnostic. The NameError
// for `nope` is the only report; the BinOp stays silent.
TEST(ExpressionTyper, OneRootCauseDrawsOneDiagnostic) {
    const Typed typed = type_expression("nope + 1");

    EXPECT_EQ(typed.diagnostics.size(), 1u);
    EXPECT_EQ(typed.diagnostics.front().code, "NameError");
    EXPECT_EQ(typed.printed, "Unknown");
}

TEST(ExpressionTyper, TypesUnaryOperators) {
    EXPECT_EQ(typed_name("-1"), "int");
    EXPECT_EQ(typed_name("-1.5"), "float");
    EXPECT_EQ(typed_name("-True"), "int") << "bool widens under arithmetic";
    EXPECT_EQ(typed_name("~1"), "int");
    EXPECT_EQ(typed_name("not 1"), "bool");
    EXPECT_EQ(typed_name("not \"s\""), "bool") << "truthiness is universal";
}

TEST(ExpressionTyper, TypesComparisonsAndChains) {
    EXPECT_EQ(typed_name("1 < 2"), "bool");
    EXPECT_EQ(typed_name("1 < 2 < 3"), "bool");
    EXPECT_EQ(typed_name("1 == \"s\""), "bool") << "equality accepts any operands";
    EXPECT_EQ(typed_name("None is None"), "bool");
}

TEST(ExpressionTyper, ReportsAnIncomparableChainLink) {
    const Typed typed = type_expression("1 < \"s\"");

    const diagnostics::Diagnostic error = only_error(typed);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "unsupported operand types for < (\"int\" and \"str\")");
}

// A chain is n comparisons. Each failing link is its own root cause, so two
// bad links are two diagnostics -- and the chain itself must not add a third.
TEST(ExpressionTyper, EachFailingChainLinkIsItsOwnRootCause) {
    const Typed typed = type_expression("1 < \"s\" < 2");

    EXPECT_EQ(typed.diagnostics.size(), 2u);
    EXPECT_EQ(typed.printed, "bool") << "a comparison is always bool, even when a link failed";
}

TEST(ExpressionTyper, TypesBooleanOperators) {
    EXPECT_EQ(typed_name("1 and 2"), "int");
    EXPECT_EQ(typed_name("1 and \"s\""), "int | str");
    EXPECT_EQ(typed_name("True or False"), "bool");
}

// Verified: mypy --strict ACCEPTS a: int = 9223372036854775808. Python
// integers are arbitrary precision. So any range check fires on a program
// mypy accepts, and the code must therefore be OverflowError -- a CAPABILITY
// diagnostic -- not TypeError. Reporting TypeError here would violate the
// project's own hard invariant inside its own literal checker.
TEST(ExpressionTyper, ReportsAnOversizedIntegerLiteralAsOverflowNotTypeError) {
    const Typed typed = type_expression("9223372036854775808");

    const diagnostics::Diagnostic error = only_error(typed);
    EXPECT_EQ(error.code, "OverflowError");
    EXPECT_EQ(error.message, "integer literal is too large for a 64-bit integer");
}

// The sign lives in the UnaryOp, and the magnitude predicate is bounded at
// 2^63 precisely so that whoever holds the sign applies it. -2^63 IS
// representable; +2^63 is not.
TEST(ExpressionTyper, TheSignIsAppliedByTheTyperNotTheLexeme) {
    EXPECT_EQ(typed_name("-9223372036854775808"), "int") << "-2^63 is representable";
    EXPECT_EQ(typed_name("9223372036854775807"), "int") << "2^63 - 1 is representable";

    const Typed positive = type_expression("9223372036854775808");
    EXPECT_EQ(only_error(positive).code, "OverflowError") << "+2^63 is not";
}

// Every typed expression gets an entry; nothing else does.
TEST(ExpressionTyper, RecordsEverySubexpressionInTheTypeMap) {
    const Typed typed = type_expression("1 + 2");

    // BinOp, Constant 1, Constant 2.
    EXPECT_EQ(typed.map_size, 3u);
}

} // namespace
} // namespace cythonpp::domain::semantic
