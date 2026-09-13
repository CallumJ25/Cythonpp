#include <map>
#include <memory>
#include <optional>
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
#include "domain/semantic/narrowing_map.h"
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
    // Every fixture here narrows nothing, so an empty map changes none of
    // their expectations -- it exists only to satisfy the constructor's
    // shape, which now matches TypeChecker's own.
    NarrowingMap narrowings;
    diagnostics::DiagnosticSink sink;
    Typed result;
    result.type = ExpressionTyper(scopes, classes, types, narrowings, sink)
                      .type_of(statement->value(), expected);
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

// Corpus defect 2 (2026-09-07): `x: type = int` is mypy-clean (measured,
// mypy 1.18.1: `Success`), but a bare builtin type name used as a VALUE (not
// an annotation, not a call) resolved through nothing but ScopeStack, which
// never holds these names, so this drew a false `NameError: name 'int' is
// not defined`. Resolved to Class("type") instead, the closest representable
// stand-in: mypy's own answer for `reveal_type(int)` is the OVERLOADED
// constructor (`Overload(def (str | Buffer | ... = ) -> int, ...)`), which
// this model cannot represent at all. Deliberately NOT the constructor
// treatment the user-class case below gets -- `int` is a model KIND, so
// there is no ClassTable entry to take a constructor from.
TEST(ExpressionTyper, ResolvesABareBuiltinTypeNameAsAValue) {
    const Typed typed = type_expression("int");

    EXPECT_TRUE(typed.diagnostics.empty())
        << "expected no diagnostics, got "
        << (typed.diagnostics.empty() ? "" : typed.diagnostics.front().message);
    EXPECT_EQ(typed.printed, "type");
}

// PRECEDENCE, pinned explicitly per the fix brief: a live SCOPE BINDING of
// the same spelling as a builtin type name must win over the builtin-type-
// name-as-value path -- `def f(int: str) -> None: print(int)` types `int` as
// `str`, not as Class("type"). Simulated here the same way every other
// shadowing test in this file does, via the bindings map.
TEST(ExpressionTyper, ALocalBindingNamedLikeABuiltinTypeWinsOverTheBuiltinPath) {
    EXPECT_EQ(typed_name("int", {{"int", Type::str()}}), "str");
}

// A USER CLASS name used as a VALUE (`w = Widget`) is
// mypy-clean but drew `NameError: name 'Widget' is not defined`, because
// class names are deliberately never bound into ScopeStack (two precedence
// checks depend on their absence) so a resolution miss is a class name's
// NORMAL state. Resolved through ClassTable to the class's CONSTRUCTOR --
// measured, mypy 1.18.1: `reveal_type(Widget)` is `def () -> Widget`, NOT
// `type[Widget]`, and the same for a nested (`def () -> Outer.Inner`) or
// function-local (`def () -> Local@6`) class. Two sibling sites already
// chose the constructor for the same reason (type_of_attribute's
// nested-class branch, type_of_name_call's bare-`C()` callee).
TEST(ExpressionTyper, ResolvesAUserClassNameUsedAsAValue) {
    ClassTable table;
    table.declare("Widget", {});

    const Typed typed = type_expression("Widget", {}, Type::unknown(), &table);

    EXPECT_TRUE(typed.diagnostics.empty())
        << "expected no diagnostics, got "
        << (typed.diagnostics.empty() ? "" : typed.diagnostics.front().message);
    EXPECT_EQ(typed.printed, "Callable[[], Widget]");
}

// The constructor is the REAL one, parameters and all, not a nullary
// stand-in: this is what makes `w = Widget` then `w(1)` check its arguments
// (and `w()` report arity) exactly as `Widget(1)` does. A test asserting
// only `Callable[[], Widget]` above would pass just as well against a
// hard-coded `Type::callable({Type::class_of(name)})`.
TEST(ExpressionTyper, AUserClassValueCarriesItsConstructorParameters) {
    ClassTable table;
    table.declare("Widget", {});
    table.declare_method("Widget", "__init__",
                         Type::callable({Type::class_of("Widget"), Type::int_()},
                                        Type::none()));

    const Typed typed = type_expression("Widget", {}, Type::unknown(), &table);

    EXPECT_TRUE(typed.diagnostics.empty())
        << "expected no diagnostics, got "
        << (typed.diagnostics.empty() ? "" : typed.diagnostics.front().message);
    EXPECT_EQ(typed.printed, "Callable[[int], Widget]")
        << "self stripped, the return replaced by the instance type";
}

// A FUNCTION-LOCAL class is declared under a synthetic isolated name and
// reached by a scope-limited alias, so the bare spelling only resolves
// through ClassTable::canonical_name. Passing the bare identifier (which is
// what type_of_name_call passes too, and what constructor_type
// canonicalises internally) is what makes this work; passing an
// already-canonical name would not have this case to get wrong.
TEST(ExpressionTyper, ResolvesAFunctionLocalClassNameUsedAsAValue) {
    ClassTable table;
    table.declare("outer#Local", {});
    const std::optional<std::string> saved = table.declare_scoped_alias("Local", "outer#Local");
    EXPECT_FALSE(saved.has_value()) << "no prior alias of this spelling";

    const Typed typed = type_expression("Local", {}, Type::unknown(), &table);

    EXPECT_TRUE(typed.diagnostics.empty())
        << "expected no diagnostics, got "
        << (typed.diagnostics.empty() ? "" : typed.diagnostics.front().message);
    EXPECT_EQ(typed.printed, "Callable[[], outer#Local]");
}

// PRECEDENCE HAZARD, pinned: a live SCOPE BINDING of the same spelling as a
// class must still win -- `def f(Widget: int) -> int: return
// Widget.bit_length()` types `Widget` as `int`, not as the class object.
// This is what placing the carve-out INSIDE the `resolution.binding ==
// nullptr` branch buys; hoisting it out would silently invert this.
TEST(ExpressionTyper, ALocalBindingNamedLikeAClassWinsOverTheClassValuePath) {
    ClassTable table;
    table.declare("Widget", {});

    const Typed typed = type_expression("Widget", {{"Widget", Type::int_()}}, Type::unknown(),
                                        &table);

    EXPECT_TRUE(typed.diagnostics.empty())
        << "expected no diagnostics, got "
        << (typed.diagnostics.empty() ? "" : typed.diagnostics.front().message);
    EXPECT_EQ(typed.printed, "int");
}

// A name that is neither bound, nor a builtin type name, nor a class is
// STILL a NameError -- the guard that the carve-out above did not simply
// swallow the diagnostic for every unresolved name.
TEST(ExpressionTyper, ANameThatIsNotAClassIsStillUndefined) {
    ClassTable table;
    table.declare("Widget", {});

    const Typed typed = type_expression("Gadget", {}, Type::unknown(), &table);

    const diagnostics::Diagnostic error = only_error(typed);
    EXPECT_EQ(error.code, "NameError");
    EXPECT_EQ(error.message, "name 'Gadget' is not defined");
}

// Two NEIGHBOURING paths this carve-out must not disturb are pinned by
// tests that already exist rather than by copies here, which would be
// vacuous: a CALL on a bare class name goes through type_of_name_call, never
// type_of_name (ConstructsAUserClass, below), and a SEEDED BUILTIN class
// name -- range/int/list are all in ClassTable too -- still takes the
// builtin-call arm (TypesTheSupportedBuiltinCalls, below). Neither routes a
// callee through type_of_name, which is the only function this fix touched.

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

// The UnaryOp(-, Constant LITERAL_INT) special case bypasses type_of() for
// its operand (see type_of_unary_op) and inserts the operand's TypeMap entry
// by hand instead. Pin that the hand-insertion actually happens, the same
// way RecordsEverySubexpressionInTheTypeMap pins the ordinary path.
TEST(ExpressionTyper, RecordsTheNegatedConstantInTheTypeMapToo) {
    const Typed typed = type_expression("-9223372036854775808");

    // UnaryOp, Constant.
    EXPECT_EQ(typed.map_size, 2u);
}

// `not` is TOTAL: always Bool for every operand type, and operator_rules.cpp
// documents that unary_result checks OP_NOT before the Unknown guard so it
// must NOT absorb Unknown. That totality lives in operator_rules.cpp, not
// here -- but these are what catch a regression where ExpressionTyper itself
// started absorbing Unknown (or gating on Class/Union) before ever calling
// unary_result.
TEST(ExpressionTyper, NotIsTotalAndNeverAbsorbsUnknownClassOrUnion) {
    const Typed unbound = type_expression("not nope");
    EXPECT_EQ(unbound.printed, "bool") << "not must stay bool, not collapse to Unknown";
    const diagnostics::Diagnostic error = only_error(unbound);
    EXPECT_EQ(error.code, "NameError") << "the NameError is nope's, not not's";

    EXPECT_EQ(typed_name("not w", {{"w", Type::class_of("Widget")}}), "bool")
        << "truthiness needs no dunder, verified mypy-clean on a plain class";
    EXPECT_EQ(typed_name("not u", {{"u", Type::union_of({Type::int_(), Type::none()})}}), "bool")
        << "truthiness is total over a union operand too";
}

// See type_of_compare's comment: a BinOp reports at the whole expression
// (one operator, one useful anchor) while a Compare chain reports at the
// failing link's operand (each link is its own root cause). Pinned here so
// the divergence is a deliberate, tested choice rather than an accident the
// Task 25 corpus would otherwise lock in unexamined.
TEST(ExpressionTyper, BinOpReportsAtTheWholeExpressionButCompareReportsAtTheFailingOperand) {
    const Typed bin_op = type_expression("1 + \"s\"");
    const diagnostics::Diagnostic bin_op_error = only_error(bin_op);
    EXPECT_EQ(bin_op_error.line, 1);
    EXPECT_EQ(bin_op_error.column, 1) << "column of the whole `1 + \"s\"` expression";

    const Typed compare = type_expression("1 < \"s\"");
    const diagnostics::Diagnostic compare_error = only_error(compare);
    EXPECT_EQ(compare_error.line, 1);
    EXPECT_EQ(compare_error.column, 5) << "column of the failing \"s\" operand, not the chain start";
}

// The UnaryOp TypeError wording was invented but never test-driven in this
// task's original pass. Pin it now, before Task 25's corpus matches it
// character for character.
TEST(ExpressionTyper, ReportsAGenuineUnaryOperandTypeError) {
    const Typed typed = type_expression("-\"s\"");

    const diagnostics::Diagnostic error = only_error(typed);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "unsupported operand type for unary - (\"str\")");
}

// Verified: reveal_type([1, 2]) is list[int]; [1, 1.5] is list[float];
// [1, True] is list[int]; [1, "s"] is list[object]; [1, None] is
// list[int | None].
TEST(ExpressionTyper, JoinsUnannotatedListElements) {
    EXPECT_EQ(typed_name("[1, 2]"), "list[int]");
    EXPECT_EQ(typed_name("[1, 1.5]"), "list[float]");
    EXPECT_EQ(typed_name("[1, True]"), "list[int]");
    EXPECT_EQ(typed_name("[1, \"s\"]"), "list[object]");
    EXPECT_EQ(typed_name("[1, None]"), "list[int | None]");
}

// Verified: reveal_type([[1], ["a"]]) is list[object], NOT
// list[list[object]] -- the join is not recursive into invariant arguments.
TEST(ExpressionTyper, DoesNotJoinRecursivelyIntoNestedDisplays) {
    EXPECT_EQ(typed_name("[[1], [2]]"), "list[list[int]]");
    EXPECT_EQ(typed_name("[[1], [\"a\"]]"), "list[object]");
}

// WITH CONTEXT there is no join: each element is checked against the declared
// element type and the result is the DECLARED type.
TEST(ExpressionTyper, TypeContextIsUsedInsteadOfAJoin) {
    const Typed floats =
        type_expression("[1, 2]", {}, Type::list_of(Type::float_()));
    EXPECT_TRUE(floats.diagnostics.empty());
    EXPECT_EQ(type_name(floats.type), "list[float]") << "declared, not joined to int";

    const Typed unions = type_expression(
        "[1, \"s\"]", {}, Type::list_of(Type::union_of({Type::int_(), Type::str()})));
    EXPECT_TRUE(unions.diagnostics.empty())
        << "an annotation makes the union work where inference would join to object";
    EXPECT_EQ(type_name(unions.type), "list[int | str]");
}

// Verified: x: list[int] = [1, "s"] reports
// 'List item 1 has incompatible type "str"; expected "int"' -- per item,
// naming the INDEX.
TEST(ExpressionTyper, ReportsPerItemAgainstTheDeclaredElementType) {
    const Typed typed = type_expression("[1, \"s\"]", {}, Type::list_of(Type::int_()));

    const diagnostics::Diagnostic error = only_error(typed);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "list item 1 has incompatible type \"str\"; expected \"int\"");
    EXPECT_EQ(type_name(typed.type), "list[int]") << "the declared type still comes back";
}

TEST(ExpressionTyper, EachBadListItemIsItsOwnRootCause) {
    const Typed typed =
        type_expression("[\"a\", 1, \"b\"]", {}, Type::list_of(Type::int_()));

    ASSERT_EQ(typed.diagnostics.size(), 2u) << "items 0 and 2";
    // Assert the actual two messages, not just a count of 2 -- an
    // implementation that reported item 0 twice (and skipped item 2 entirely)
    // would also satisfy size() == 2.
    EXPECT_EQ(typed.diagnostics[0].message,
              "list item 0 has incompatible type \"str\"; expected \"int\"");
    EXPECT_EQ(typed.diagnostics[1].message,
              "list item 2 has incompatible type \"str\"; expected \"int\"");
}

// Type context propagates recursively. Verified both clean.
TEST(ExpressionTyper, TypeContextPropagatesIntoNestedDisplays) {
    const Typed nested = type_expression(
        "[[1], [2]]", {}, Type::list_of(Type::list_of(Type::int_())));
    EXPECT_TRUE(nested.diagnostics.empty());
    EXPECT_EQ(type_name(nested.type), "list[list[int]]");
}

// Negative control for the propagation above: a bad element inside a NESTED
// display must report exactly once, at the inner arm that actually checked
// it, and the outer arm must not pile on a second diagnostic. This holds
// because the inner list arm always returns the DECLARED type (list[int])
// even when one of its own elements was bad, so from the outer arm's own
// is_subtype check the child still looks like a perfect match. Without this
// test, a regression that made the outer arm re-check the inner element
// against its own actual (joined) type -- or that made the inner arm report
// twice -- would pass unnoticed.
TEST(ExpressionTyper, ANestedDisplayMismatchReportsOnceAtTheInnerDepth) {
    const Typed typed = type_expression(
        "[[1], [\"s\"]]", {}, Type::list_of(Type::list_of(Type::int_())));

    ASSERT_EQ(typed.diagnostics.size(), 1u)
        << "the inner mismatch must report once; the outer arm must not add a second";
    EXPECT_EQ(typed.diagnostics.front().message,
              "list item 0 has incompatible type \"str\"; expected \"int\"")
        << "reported by the INNER list, naming the inner index -- not the outer index (1)";
    EXPECT_EQ(type_name(typed.type), "list[list[int]]")
        << "the declared type still comes back, even with a nested failure";
}

// Verified: reveal_type({1: "a", 2: "b"}) is dict[int, str];
// {1: "a", "k": "b"} is dict[object, str]; {1: "a", 2: 3} is
// dict[int, object]. Keys and values join INDEPENDENTLY.
TEST(ExpressionTyper, JoinsDictKeysAndValuesIndependently) {
    EXPECT_EQ(typed_name("{1: \"a\", 2: \"b\"}"), "dict[int, str]");
    EXPECT_EQ(typed_name("{1: \"a\", \"k\": \"b\"}"), "dict[object, str]");
    EXPECT_EQ(typed_name("{1: \"a\", 2: 3}"), "dict[int, object]");
}

// The dict-with-context error path (type_of_dict's has_context branch) had
// NO test at all at one point -- unreachable from the whole suite.
// Ground truth from real mypy 1.18.1: `x: dict[str,int] = {1: 2}` reports
// 'Dict entry 0 has incompatible type "int": "int"; expected "str": "int"'
// -- note the VALUE side ("int": "int") matches perfectly and is still
// quoted, because mypy (and this project) reports one diagnostic per bad
// ENTRY, not one per bad half. Here only the key (1, an int) is actually
// wrong against the declared str key type; the value (2) already satisfies
// the declared int value type.
TEST(ExpressionTyper, ReportsADictEntryWithOnlyTheKeyWrong) {
    const Typed typed = type_expression(
        "{1: 2}", {}, Type::dict_of(Type::str(), Type::int_()));

    const diagnostics::Diagnostic error = only_error(typed);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message,
              "dict entry 0 has incompatible type \"int\": \"int\"; expected \"str\": \"int\"");
    EXPECT_EQ(type_name(typed.type), "dict[str, int]") << "the declared type still comes back";
    // Pin the anchor position too, now that this path finally has coverage:
    // the diagnostic is reported at the KEY's own span, not the whole entry
    // or the dict expression. Source is "{1: 2}\n" -- '{' is column 1, the
    // key '1' is column 2.
    EXPECT_EQ(error.line, 1);
    EXPECT_EQ(error.column, 2) << "anchored at the key's span, not the whole entry";
}

// Mirror case: only the VALUE is wrong (key already satisfies its declared
// type). Still one diagnostic, still quoting both actual halves together.
TEST(ExpressionTyper, ReportsADictEntryWithOnlyTheValueWrong) {
    const Typed typed = type_expression(
        "{\"a\": \"b\"}", {}, Type::dict_of(Type::str(), Type::int_()));

    const diagnostics::Diagnostic error = only_error(typed);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message,
              "dict entry 0 has incompatible type \"str\": \"str\"; expected \"str\": \"int\"");
    EXPECT_EQ(type_name(typed.type), "dict[str, int]");
}

// Both halves wrong at once: still exactly ONE diagnostic for the entry, not
// two (one per bad half) -- a decision that had no test of its own.
TEST(ExpressionTyper, ReportsADictEntryWithBothHalvesWrongAsOneDiagnostic) {
    const Typed typed = type_expression(
        "{1: \"x\"}", {}, Type::dict_of(Type::str(), Type::int_()));

    const diagnostics::Diagnostic error = only_error(typed);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message,
              "dict entry 0 has incompatible type \"int\": \"str\"; expected \"str\": \"int\"");
}

// Verified: reveal_type((1, "s")) keeps positional element types -- tuples
// are heterogeneous and there is NO join.
TEST(ExpressionTyper, TuplesKeepTheirPositionalElementTypes) {
    EXPECT_EQ(typed_name("(1, \"s\")"), "tuple[int, str]");
    EXPECT_EQ(typed_name("(1, 1.5)"), "tuple[int, float]");
}

// Ground truth from real mypy 1.18.1: mypy has NO per-item tuple diagnostic.
// `x: tuple[int, str] = (1, 2)` reports ONE `assignment` error naming the two
// whole tuple types ("expression has type \"tuple[int, int]\", variable has
// type \"tuple[int, str]\""), not a per-element TypeError. So, unlike
// List/Dict, TupleExpr's context branch never reports here -- it stays
// silent even on a genuine element mismatch, and returns the POSITIONAL
// types actually present (tuple[int, int], NOT the declared tuple[int, str])
// so the later assignment check is the one place the mismatch surfaces.
TEST(ExpressionTyper, AContextMismatchedTupleElementIsSilentAndReturnsPositionalTypes) {
    const Typed typed = type_expression(
        "(1, 2)", {}, Type::tuple_of({Type::int_(), Type::str()}));

    EXPECT_TRUE(typed.diagnostics.empty())
        << "the assignment check reports the whole-tuple mismatch, not the display";
    EXPECT_EQ(type_name(typed.type), "tuple[int, int]")
        << "positional types come back, NOT the declared tuple[int, str]";
}

// Ground truth from real mypy 1.18.1: a wrong-ARITY tuple context
// (`x: tuple[int, str] = (1,)`) is ALSO just one `assignment` error naming
// "tuple[int]" vs "tuple[int, str]" -- the same single-diagnostic shape as
// the element mismatch above, not a distinct "arity mismatch" report. Since
// the arity differs, has_context is false here (see type_of_tuple), so this
// is really the ordinary no-context path, but it is pinned explicitly
// because it is the other half of the mypy ground truth this task settles.
TEST(ExpressionTyper, AWrongArityTupleContextIsSilentAndReturnsPositionalTypes) {
    const Typed typed = type_expression(
        "(1,)", {}, Type::tuple_of({Type::int_(), Type::str()}));

    EXPECT_TRUE(typed.diagnostics.empty())
        << "the assignment check reports the whole-tuple mismatch, not the display";
    EXPECT_EQ(type_name(typed.type), "tuple[int]");
}

// Verified: reveal_type(()) is tuple[()] and it is CLEAN -- the one display
// that needs no annotation, because tuple[()] is a complete non-generic type.
TEST(ExpressionTyper, TheEmptyTupleNeedsNoAnnotation) {
    const Typed typed = type_expression("()");

    EXPECT_TRUE(typed.diagnostics.empty());
    EXPECT_EQ(typed.printed, "tuple[()]");
}

// Verified: bare x = [] is 'Need type annotation for "x"' [var-annotated] --
// a genuine mypy error, so TypeError is correct and in direction (b).
// ExpressionTyper does NOT report here: mypy types a bare `[]` in EXPRESSION
// position as list[Never] and says nothing. The var-annotated error belongs
// to the ASSIGNMENT, which is the only place the variable's name exists to
// put in the message. So the typer returns Unknown silently and Task 17's
// Assign arm reports.
TEST(ExpressionTyper, AnEmptyDisplayWithNoContextIsSilentlyUnknown) {
    const Typed list = type_expression("[]");
    EXPECT_TRUE(list.diagnostics.empty())
        << "the assignment reports, not the display -- mypy types a bare [] as list[Never]";
    EXPECT_EQ(list.printed, "Unknown");

    // Same reasoning applies to a bare `{}`: mypy types it as dict[Never,
    // Never] in expression position and reports nothing, so the same
    // var-annotated error belongs to the assignment, not the display.
    const Typed dict = type_expression("{}");
    EXPECT_TRUE(dict.diagnostics.empty());
    EXPECT_EQ(dict.printed, "Unknown");
}

TEST(ExpressionTyper, AnEmptyDisplayWithContextTakesTheContext) {
    const Typed list = type_expression("[]", {}, Type::list_of(Type::int_()));
    EXPECT_TRUE(list.diagnostics.empty());
    EXPECT_EQ(type_name(list.type), "list[int]");

    const Typed dict =
        type_expression("{}", {}, Type::dict_of(Type::str(), Type::int_()));
    EXPECT_TRUE(dict.diagnostics.empty());
    EXPECT_EQ(type_name(dict.type), "dict[str, int]");
}

// A context of the WRONG SHAPE must not be silently adopted: x: int = [1]
// is a genuine error, and it is the statement checker's to report, so the
// display arm falls back to inference rather than pretending the context fits.
TEST(ExpressionTyper, AContextOfTheWrongShapeFallsBackToInference) {
    const Typed typed = type_expression("[1, 2]", {}, Type::int_());

    EXPECT_TRUE(typed.diagnostics.empty()) << "the assignment check reports, not the display";
    EXPECT_EQ(type_name(typed.type), "list[int]");
}

// Coverage gap left by Task 13: no test exercised a TUPLE context
// propagating into a NESTED empty display, only a top-level one. The inner
// `[]` must resolve to list[int] from the positional element context
// (type_of_tuple threads `element_expected` into its recursive type_of()
// call), not fall to the no-context path and go silently Unknown.
TEST(ExpressionTyper, TupleContextPropagatesIntoANestedEmptyDisplay) {
    const Typed typed =
        type_expression("([],)", {}, Type::tuple_of({Type::list_of(Type::int_())}));

    EXPECT_TRUE(typed.diagnostics.empty());
    EXPECT_EQ(typed.printed, "tuple[list[int]]");
}

// --- Subscript (Task 14) ---------------------------------------------------

TEST(ExpressionTyper, TypesSubscriptThroughTheRuleTable) {
    EXPECT_EQ(typed_name("xs[0]", {{"xs", Type::list_of(Type::str())}}), "str");
    EXPECT_EQ(typed_name("bs[0]", {{"bs", Type::bytes()}}), "int") << "bytes[int] is int";
    EXPECT_EQ(typed_name("d[\"k\"]", {{"d", Type::dict_of(Type::str(), Type::int_())}}), "int");
    // An integer LITERAL index resolves a heterogeneous tuple element-wise.
    // Verified against mypy 1.18.1: reveal_type(t[0]) for `t: tuple[int, str]`
    // is `builtins.int`.
    EXPECT_EQ(typed_name("t[0]", {{"t", Type::tuple_of({Type::int_(), Type::str()})}}), "int")
        << "a literal index selects that one element";
    // A VARIABLE index still yields the union -- mypy's element-wise rule
    // depends on the index's TYPE being literal, which this grammar has no
    // way to express for anything but the literal syntax itself. Verified:
    // reveal_type(t[i]) for `i: int` is `builtins.int | builtins.str`.
    EXPECT_EQ(typed_name("t[i]", {{"t", Type::tuple_of({Type::int_(), Type::str()})},
                                  {"i", Type::int_()}}),
              "int | str")
        << "a variable index still yields the union";
}

// --- Which index SPELLINGS count as a literal ------------------------------
//
// One test per form, deliberately, rather than one combined case: the
// extractor's failure mode is SILENT -- a wrong parse selects the wrong
// element with no diagnostic at all -- so every form needs its own pinned
// expectation. The fixture is a three-element tuple throughout so that 0, 1
// and -1 are three DIFFERENT answers; on a pair, `t[--1]` and `t[+-1]` would
// agree by accident.
//
// Every expectation below is what mypy 1.18.1 reveals for the same index on
// `t: tuple[int, str, float]`.

const std::map<std::string, Type> kTriple = {
    {"t", Type::tuple_of({Type::int_(), Type::str(), Type::float_()})}};

// reveal_type(t[0x1]) and reveal_type(t[0X1]) are both `builtins.str`.
TEST(ExpressionTyper, AHexadecimalLiteralIndexSelectsOneTupleElement) {
    EXPECT_EQ(typed_name("t[0x1]", kTriple), "str");
    EXPECT_EQ(typed_name("t[0X2]", kTriple), "float");
}

// reveal_type(t[0o1]) and reveal_type(t[0O1]) are both `builtins.str`.
// Base 8 is read explicitly: C's strtoll with base 0 would take `0o1` as the
// decimal 0 followed by junk.
TEST(ExpressionTyper, AnOctalLiteralIndexSelectsOneTupleElement) {
    EXPECT_EQ(typed_name("t[0o1]", kTriple), "str");
    EXPECT_EQ(typed_name("t[0O2]", kTriple), "float");
}

// reveal_type(t[0b1]) and reveal_type(t[0B1]) are both `builtins.str`.
TEST(ExpressionTyper, ABinaryLiteralIndexSelectsOneTupleElement) {
    EXPECT_EQ(typed_name("t[0b1]", kTriple), "str");
    EXPECT_EQ(typed_name("t[0b10]", kTriple), "float");
}

// A plain leading zero is NOT octal in Python -- `00` is zero, and `010` is a
// syntax error the lexer/parser never hands here. This pins that the base
// detection keys on the letter, not on the leading zero, so `00` stays 0.
TEST(ExpressionTyper, ALeadingZeroIsNotReadAsOctal) {
    EXPECT_EQ(typed_name("t[00]", kTriple), "int");
}

// Digit separators are stripped: reveal_type(t[0x_1]) is `builtins.str`, and
// reveal_type(t[1_0]) on an eleven-element tuple is that element -- so `1_0`
// is ten, not one.
TEST(ExpressionTyper, DigitSeparatorsAreStrippedFromALiteralIndex) {
    EXPECT_EQ(typed_name("t[0x_1]", kTriple), "str");
    EXPECT_EQ(typed_name("t[0b1_0]", kTriple), "float");
}

// A BARE bool is a literal index: reveal_type(t[True]) is `builtins.str` and
// reveal_type(t[False]) is `builtins.int`. `bool` is an `int` subtype and
// `True`/`False` carry a Literal type of their own.
TEST(ExpressionTyper, ABareBooleanIndexSelectsOneTupleElement) {
    EXPECT_EQ(typed_name("t[True]", kTriple), "str");
    EXPECT_EQ(typed_name("t[False]", kTriple), "int");
}

// A bool under ANY unary operator loses its literalness. Measured: `t[-True]`,
// `t[+True]`, `t[-False]`, `t[+False]` and `t[--True]` ALL reveal
// `builtins.int | builtins.str | builtins.float`, so the union is mypy's own
// answer here and matching it means declining.
TEST(ExpressionTyper, ABooleanUnderAUnaryOperatorIsNotALiteralIndex) {
    EXPECT_EQ(typed_name("t[-True]", kTriple), "int | str | float");
    EXPECT_EQ(typed_name("t[+True]", kTriple), "int | str | float");
    EXPECT_EQ(typed_name("t[-False]", kTriple), "int | str | float");
    EXPECT_EQ(typed_name("t[--True]", kTriple), "int | str | float");
}

// Unary PLUS, not only unary minus: reveal_type(t[+0]) is `builtins.int` and
// reveal_type(t[+1]) is `builtins.str`.
TEST(ExpressionTyper, AUnaryPlusLiteralIndexSelectsOneTupleElement) {
    EXPECT_EQ(typed_name("t[+0]", kTriple), "int");
    EXPECT_EQ(typed_name("t[+1]", kTriple), "str");
}

// NESTED unary operators fold arithmetically. Measured: reveal_type(t[--1])
// and reveal_type(t[++1]) are `builtins.str`; reveal_type(t[+-1]),
// reveal_type(t[-+1]) and reveal_type(t[---1]) are `builtins.float`.
TEST(ExpressionTyper, NestedUnaryOperatorsFoldInALiteralIndex) {
    EXPECT_EQ(typed_name("t[--1]", kTriple), "str");
    EXPECT_EQ(typed_name("t[++1]", kTriple), "str");
    EXPECT_EQ(typed_name("t[+-1]", kTriple), "float");
    EXPECT_EQ(typed_name("t[-+1]", kTriple), "float");
    EXPECT_EQ(typed_name("t[---1]", kTriple), "float");
    // A negated hexadecimal, so the two mechanisms compose: -0x1 is -1.
    EXPECT_EQ(typed_name("t[-0x1]", kTriple), "float");
}

// `~` is DECLINED, and that is the one index form the union is right for:
// reveal_type(t[~0]) is `builtins.int | builtins.str | builtins.float`,
// because `~` yields a plain `int` rather than a `Literal`.
TEST(ExpressionTyper, ABitwiseNotIndexIsNotALiteralIndex) {
    EXPECT_EQ(typed_name("t[~0]", kTriple), "int | str | float");
}

// Reading a separator-bearing literal also makes an OUT OF RANGE one
// reportable, where declining it left us silent. mypy agrees: `t[1_0]` on a
// tuple[int, str, float] is `error: Tuple index out of range  [misc]`, and
// CPython raises `IndexError: tuple index out of range`.
TEST(ExpressionTyper, AnOutOfRangeSeparatedLiteralIndexIsReported) {
    const Typed typed = type_expression("t[1_0]", kTriple);

    const diagnostics::Diagnostic error = only_error(typed);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "tuple index out of range");
}

// An EMPTY tuple with a literal index gets the OUT OF RANGE wording, not the
// index-type wording: `int` is the right KIND for a tuple index, and the real
// fault is that every index is out of range in a zero-length tuple. mypy:
// `error: Tuple index out of range  [misc]`; CPython: `IndexError: tuple
// index out of range`.
TEST(ExpressionTyper, AnEmptyTupleWithALiteralIndexIsReportedAsOutOfRange) {
    const Typed typed = type_expression("t[0]", {{"t", Type::tuple_of({})}});

    const diagnostics::Diagnostic error = only_error(typed);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "tuple index out of range");
}

// ...but a VARIABLE index on the empty tuple keeps the index-type wording.
// Measured: mypy is CLEAN there (`reveal_type(e[j])` for `j: int` is
// `Never`), so this report is this compiler's own stricter call, and the
// out-of-range claim would be a claim mypy does not make.
TEST(ExpressionTyper, AnEmptyTupleWithAVariableIndexKeepsTheIndexTypeWording) {
    const Typed typed = type_expression("t[i]", {{"t", Type::tuple_of({})},
                                                 {"i", Type::int_()}});

    const diagnostics::Diagnostic error = only_error(typed);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "invalid index type \"int\" for \"tuple[()]\"");
}

TEST(ExpressionTyper, ReportsABadIndexType) {
    const Typed typed =
        type_expression("d[1]", {{"d", Type::dict_of(Type::str(), Type::int_())}});

    const diagnostics::Diagnostic error = only_error(typed);
    EXPECT_EQ(error.code, "TypeError");
}

// Verified: `s[0]` with __getitem__(self, i: int) -> str is mypy-CLEAN, so a
// TypeError here would be false.
TEST(ExpressionTyper, ReportsSubscriptingAUserClassAsUnsupported) {
    ClassTable table;
    table.declare("Widget", {});
    const Typed typed =
        type_expression("w[0]", {{"w", Type::class_of("Widget")}}, Type::unknown(), &table);

    EXPECT_EQ(only_error(typed).code, "NotImplementedError");
}

// Absorbing: an unbound container's NameError is the only report.
// subscript_result returns Ok(Unknown) whenever either operand is Unknown
// (operator_rules.cpp), and apply()'s Ok arm never reports -- so this pins
// that a miss upstream draws exactly ONE diagnostic, not a second false
// TypeError/NotImplementedError layered on top.
TEST(ExpressionTyper, SubscriptSilentlyAbsorbsAnUnboundContainer) {
    const Typed typed = type_expression("nope[0]");

    EXPECT_EQ(typed.diagnostics.size(), 1u);
    EXPECT_EQ(typed.diagnostics.front().code, "NameError");
    EXPECT_EQ(typed.printed, "Unknown");
}

// --- Attribute (Task 14) ----------------------------------------------------

TEST(ExpressionTyper, ResolvesAUserClassAttribute) {
    ClassTable table;
    table.declare("Widget", {});
    table.declare_member("Widget", "width", Type::int_(), 2);

    const Typed typed = type_expression("w.width", {{"w", Type::class_of("Widget")}},
                                        Type::unknown(), &table);
    EXPECT_TRUE(typed.diagnostics.empty());
    EXPECT_EQ(typed.printed, "int");
}

TEST(ExpressionTyper, ResolvesAnInheritedAttribute) {
    ClassTable table;
    table.declare("Base", {});
    table.declare_member("Base", "x", Type::str(), 2);
    table.declare("Leaf", {Type::class_of("Base")});

    const Typed typed =
        type_expression("w.x", {{"w", Type::class_of("Leaf")}}, Type::unknown(), &table);
    EXPECT_TRUE(typed.diagnostics.empty());
    EXPECT_EQ(typed.printed, "str");
}

// ClassTable has TWO member queries -- member_type (Entry::members) and
// method_type (Entry::methods) -- and a bare `c.m` reference (never called)
// must resolve through method_type too, or every ordinary method reference
// would be a false attr-defined TypeError.
//
// THE self CONTRACT, verified against mypy 1.18.1: an INSTANCE receiver's
// `self` is dropped AT
// THE ATTRIBUTE ACCESS, not at a later call: `c: C = C()` then
// `reveal_type(c.m)` is `def () -> int`. So `w.resize` below -- `w`'s type
// is Widget, an instance, not the class object -- must already be BOUND
// (self gone). Task 15's Call arm must NOT drop args[0] again for an
// Attribute callee; it is already bound here. Renamed from
// ResolvesAMethodReferenceIncludingSelf, which pinned the OPPOSITE (and now
// known wrong) expectation.
TEST(ExpressionTyper, ResolvesABoundMethodReferenceWithSelfDropped) {
    ClassTable table;
    table.declare("Widget", {});
    table.declare_method("Widget", "resize",
                         Type::callable({Type::class_of("Widget"), Type::int_()}, Type::none()));

    const Typed typed = type_expression("w.resize", {{"w", Type::class_of("Widget")}},
                                        Type::unknown(), &table);
    EXPECT_TRUE(typed.diagnostics.empty());
    EXPECT_EQ(typed.printed, "Callable[[int], None]")
        << "self is dropped here, at the instance attribute access -- not by Task 15's Call arm";
}

// The CLASS-OBJECT counterpart of the test above: `Widget.resize`, receiver
// is the class itself (a bare Name satisfying classes_.is_class), not an
// instance. Verified against mypy 1.18.1: reveal_type(C.m) is
// `def (self: C) -> int` -- self stays, unlike the instance case.
TEST(ExpressionTyper, ResolvesAClassObjectMethodReferenceKeepingSelf) {
    ClassTable table;
    table.declare("Widget", {});
    table.declare_method("Widget", "resize",
                         Type::callable({Type::class_of("Widget"), Type::int_()}, Type::none()));

    const Typed typed = type_expression("Widget.resize", {}, Type::unknown(), &table);
    EXPECT_TRUE(typed.diagnostics.empty());
    EXPECT_EQ(typed.printed, "Callable[[Widget, int], None]")
        << "self stays for a class-object receiver";
}

// `C.x`: a class-object receiver accessing a plain (non-method) member.
// Verified mypy-clean, reveal_type(C.x) is builtins.int -- Decision 7 in the
// spec requires C.x to be accepted, since mypy accesses a member through
// EITHER the instance or the class.
TEST(ExpressionTyper, ResolvesAClassObjectMemberAttribute) {
    ClassTable table;
    table.declare("Widget", {});
    table.declare_member("Widget", "width", Type::int_(), 2);

    const Typed typed = type_expression("Widget.width", {}, Type::unknown(), &table);
    EXPECT_TRUE(typed.diagnostics.empty());
    EXPECT_EQ(typed.printed, "int");
}

// `C.nope`: a class-object receiver missing the attribute is a genuine mypy
// attr-defined error, exactly like the instance case.
TEST(ExpressionTyper, ReportsAMissingAttributeOnAClassObjectReceiver) {
    ClassTable table;
    table.declare("Widget", {});

    const Typed typed = type_expression("Widget.nope", {}, Type::unknown(), &table);
    const diagnostics::Diagnostic error = only_error(typed);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "\"Widget\" has no attribute \"nope\"");
}

// A LOCAL BINDING with the same name as a declared class must
// win over the class-object reading. `def f(Widget: int): return
// Widget.bit_length()` is legal Python -- `Widget` is an int parameter, not
// the class -- so the receiver must go down the ordinary VALUE path
// (scopes_.resolve() first), landing on the builtin-receiver carve-out, NOT
// a class-object member lookup (which would find no member at all, or worse,
// the wrong one).
TEST(ExpressionTyper, AShadowingVariableWinsOverAClassNameForABuiltinReceiver) {
    ClassTable table;
    table.declare("Widget", {});

    const Typed typed = type_expression("Widget.bit_length", {{"Widget", Type::int_()}},
                                        Type::unknown(), &table);
    const diagnostics::Diagnostic error = only_error(typed);
    EXPECT_EQ(error.code, "NotImplementedError");
    EXPECT_EQ(error.message, "methods on builtin types are not supported")
        << "the int parameter must win over the class name -- this must NOT be a "
           "class-object member lookup";
}

// Same shadowing bug, but with a user-class-typed binding: `Widget` the
// PARAMETER has type `Other` (with member `x`); `Widget` the CLASS (a
// distinct declaration) has a different member, `y`. If the class-object
// path won incorrectly, `Widget.x` would look up `x` on the Widget class
// (a miss, since Widget only declares `y`) instead of on Other. Asserting
// the resolved type is Other's `x` (str) -- not a TypeError -- proves the
// variable won.
TEST(ExpressionTyper, AShadowingVariableWinsOverAClassNameForAUserClassReceiver) {
    ClassTable table;
    table.declare("Other", {});
    table.declare_member("Other", "x", Type::str(), 2);
    table.declare("Widget", {});
    table.declare_member("Widget", "y", Type::int_(), 2);

    const Typed typed = type_expression("Widget.x", {{"Widget", Type::class_of("Other")}},
                                        Type::unknown(), &table);
    EXPECT_TRUE(typed.diagnostics.empty());
    EXPECT_EQ(typed.printed, "str")
        << "the shadowing variable's type (Other) must win over the class name (Widget)";
}

// A class defining __getattr__ makes ARBITRARY attribute access mypy-clean.
// Verified against mypy 1.18.1: `class G: def __getattr__(self, name: str)
// -> int: ...` then `g.anything` is mypy-CLEAN, revealing builtins.int.
// Without this, `g.anything` would draw a false attr-defined TypeError --
// the hard invariant this fix closes.
TEST(ExpressionTyper, GetattrResolvesAnArbitraryAttribute) {
    ClassTable table;
    table.declare("G", {});
    table.declare_method(
        "G", "__getattr__",
        Type::callable({Type::class_of("G"), Type::str()}, Type::int_()));

    const Typed typed =
        type_expression("g.anything", {{"g", Type::class_of("G")}}, Type::unknown(), &table);
    EXPECT_TRUE(typed.diagnostics.empty());
    EXPECT_EQ(typed.printed, "int");
}

// A DECLARED member/method still wins over __getattr__ -- the fallback is
// only consulted on a genuine miss.
TEST(ExpressionTyper, ADeclaredMemberStillWinsOverGetattr) {
    ClassTable table;
    table.declare("G", {});
    table.declare_member("G", "label", Type::str(), 2);
    table.declare_method(
        "G", "__getattr__",
        Type::callable({Type::class_of("G"), Type::str()}, Type::int_()));

    const Typed typed =
        type_expression("g.label", {{"g", Type::class_of("G")}}, Type::unknown(), &table);
    EXPECT_TRUE(typed.diagnostics.empty());
    EXPECT_EQ(typed.printed, "str") << "the declared member wins, not __getattr__'s int";
}

// mypy's attr-defined, and it IS in direction (b)'s rule set.
TEST(ExpressionTyper, ReportsAMissingAttributeOnAUserClass) {
    ClassTable table;
    table.declare("Widget", {});

    const Typed typed = type_expression("w.nope", {{"w", Type::class_of("Widget")}},
                                        Type::unknown(), &table);
    const diagnostics::Diagnostic error = only_error(typed);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "\"Widget\" has no attribute \"nope\"");
}

// THE CARVE-OUT that closing Task 9's violation forced. Verified:
// `class Sub(int): pass` then Sub().bit_length() is mypy-CLEAN, because Sub
// inherits int's members. Without this row, Task 9's fix would make every
// attribute miss on Sub a FALSE TypeError.
//
// The accepted cost: Sub().nope IS a genuine mypy attr-defined error and we
// report NotImplementedError instead -- a MISSED error, so invariant (a)
// holds and direction (b) loses attr-defined for builtin-inheriting classes
// only.
TEST(ExpressionTyper, AMissingAttributeOnABuiltinInheritingClassIsUnsupported) {
    ClassTable table;
    table.declare("Sub", {Type::int_()});

    const Typed typed = type_expression("s.bit_length", {{"s", Type::class_of("Sub")}},
                                        Type::unknown(), &table);
    const diagnostics::Diagnostic error = only_error(typed);
    EXPECT_EQ(error.code, "NotImplementedError");
    EXPECT_EQ(error.message, "methods on builtin types are not supported");
}

// A DECLARED member still wins over the carve-out.
TEST(ExpressionTyper, ADeclaredMemberOnABuiltinInheritingClassStillResolves) {
    ClassTable table;
    table.declare("Sub", {Type::int_()});
    table.declare_member("Sub", "label", Type::str(), 3);

    const Typed typed =
        type_expression("s.label", {{"s", Type::class_of("Sub")}}, Type::unknown(), &table);
    EXPECT_TRUE(typed.diagnostics.empty())
        << "an implementation that returned the member AND reported the carve-out's "
           "NotImplementedError would otherwise still pass";
    EXPECT_EQ(typed.printed, "str");
}

// inherits_builtin's sibling carve-out for a SEEDED BUILTIN CLASS ROW
// (Exception, OSError, slice, property, memoryview, type, super, ...),
// rather than a modelled TypeKind. Verified: `class MyErr(ValueError): pass`
// then `e.args` is mypy-clean (`Success`) and CPython prints `('bad',)` --
// this was a false TypeError before ClassTable::inherits_builtin_class
// existed, since inherits_builtin is deliberately false for a Class base
// (see InheritsBuiltinIsFalseForASeededExceptionBase in class_table_test.cpp).
TEST(ExpressionTyper, AMissingAttributeOnASeededBuiltinClassBaseIsUnsupported) {
    ClassTable table;
    table.declare("MyErr", {Type::class_of("ValueError")});

    const Typed typed = type_expression("e.args", {{"e", Type::class_of("MyErr")}},
                                        Type::unknown(), &table);
    const diagnostics::Diagnostic error = only_error(typed);
    EXPECT_EQ(error.code, "NotImplementedError");
    EXPECT_EQ(error.message, "methods on builtin types are not supported");
}

// The seeded row itself, with no intervening user subclass at all --
// `super().__init__()`'s receiver resolves directly to Class("super"), which
// is exactly this shape. Verified: mypy --strict is Success and CPython
// runs a subclass `__init__` calling `super().__init__()` cleanly.
TEST(ExpressionTyper, AMissingAttributeOnASeededBuiltinClassItselfIsUnsupported) {
    const ClassTable table;

    const Typed typed = type_expression("s.__init__", {{"s", Type::class_of("super")}},
                                        Type::unknown(), &table);
    const diagnostics::Diagnostic error = only_error(typed);
    EXPECT_EQ(error.code, "NotImplementedError");
    EXPECT_EQ(error.message, "methods on builtin types are not supported");
}

// A DECLARED member still wins over the second carve-out too, same as the
// first (ADeclaredMemberOnABuiltinInheritingClassStillResolves above).
TEST(ExpressionTyper, ADeclaredMemberOnASeededBuiltinClassBaseStillResolves) {
    ClassTable table;
    table.declare("MyErr", {Type::class_of("ValueError")});
    table.declare_member("MyErr", "code", Type::int_(), 3);

    const Typed typed =
        type_expression("e.code", {{"e", Type::class_of("MyErr")}}, Type::unknown(), &table);
    EXPECT_TRUE(typed.diagnostics.empty())
        << "an implementation that returned the member AND reported the carve-out's "
           "NotImplementedError would otherwise still pass";
    EXPECT_EQ(typed.printed, "int");
}

// A class shadowing a builtin spelling (`class slice: pass`) must NOT gain
// this carve-out -- declare() resets Entry::is_seeded_builtin, so a genuine
// miss on it stays a genuine attr-defined TypeError, matching both oracles
// (mypy: attr-defined; CPython: AttributeError).
TEST(ExpressionTyper, AMissingAttributeOnAClassShadowingABuiltinSpellingStaysATypeError) {
    ClassTable table;
    table.declare("slice", {});

    const Typed typed = type_expression("s.nope", {{"s", Type::class_of("slice")}},
                                        Type::unknown(), &table);
    const diagnostics::Diagnostic error = only_error(typed);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "\"slice\" has no attribute \"nope\"");
}

// THE end-to-end control for the most consequential exclusion in
// inherits_builtin_class: an ordinary class with NO bases at all. Its chain
// walk never visits an `object` entry either way (there is nothing to walk),
// so this control passes whether or not the `canonical == "object"` line in
// class_table.cpp exists -- it is NOT evidence for that exclusion (see
// AMissingAttributeOnAClassWithAnExplicitObjectBaseStaysATypeError below for
// the shape that actually is). It exists anyway because both oracles reject
// this program (mypy attr-defined, CPython AttributeError) and nothing else
// in this file pins it end-to-end through the second carve-out's own report
// site.
TEST(ExpressionTyper, AMissingAttributeOnAPlainClassStaysATypeError) {
    ClassTable table;
    table.declare("Plain", {});

    const Typed typed = type_expression("p.nope", {{"p", Type::class_of("Plain")}},
                                        Type::unknown(), &table);
    const diagnostics::Diagnostic error = only_error(typed);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "\"Plain\" has no attribute \"nope\"");
    EXPECT_EQ(error.line, 1);
}

// UNLIKE the bare-Plain control above, this one DOES exercise the
// `canonical == "object"` exclusion inside inherits_builtin_class's chain
// walk: `object` is Plain's own recorded base here, so the walk visits its
// entry (is_seeded_builtin true) and the exclusion is what stops that entry
// from answering. Proven by neutering: removing the exclusion flips this
// test (and ClassTable.InheritsBuiltinClassExcludesObject) from TypeError to
// a false NotImplementedError, while
// AMissingAttributeOnAPlainClassStaysATypeError above is unaffected either
// way. Both oracles reject this program identically to the bare-Plain case
// (mypy attr-defined, CPython AttributeError).
TEST(ExpressionTyper, AMissingAttributeOnAClassWithAnExplicitObjectBaseStaysATypeError) {
    ClassTable table;
    table.declare("Plain", {Type::object()});

    const Typed typed = type_expression("p.nope", {{"p", Type::class_of("Plain")}},
                                        Type::unknown(), &table);
    const diagnostics::Diagnostic error = only_error(typed);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "\"Plain\" has no attribute \"nope\"");
    EXPECT_EQ(error.line, 1);
}

// Verified: xs.append(1), s.upper() and d.keys() are ALL mypy-clean. With no
// typeshed, reporting attr-defined here would be a false TypeError on one of
// the most common lines in Python.
TEST(ExpressionTyper, ReportsBuiltinAttributeAccessAsUnsupported) {
    for (const Type& receiver :
         {Type::list_of(Type::int_()), Type::str(), Type::dict_of(Type::str(), Type::int_())}) {
        const Typed typed = type_expression("r.anything", {{"r", receiver}});
        const diagnostics::Diagnostic error = only_error(typed);
        EXPECT_EQ(error.code, "NotImplementedError");
        EXPECT_EQ(error.message, "methods on builtin types are not supported");
    }
}

TEST(ExpressionTyper, ReportsAttributeAccessOnAUnionAsNeedingNarrowing) {
    const Typed typed = type_expression(
        "x.f", {{"x", Type::union_of({Type::class_of("A"), Type::none()})}});

    const diagnostics::Diagnostic error = only_error(typed);
    EXPECT_EQ(error.code, "NotImplementedError");
    EXPECT_EQ(error.message,
              "operations on a union-typed value require narrowing, which is not supported");
}

// Absorbing: the NameError is the only report.
TEST(ExpressionTyper, AttributeAccessOnUnknownIsSilent) {
    const Typed typed = type_expression("nope.f");

    EXPECT_EQ(typed.diagnostics.size(), 1u);
    EXPECT_EQ(typed.diagnostics.front().code, "NameError");
    EXPECT_EQ(typed.printed, "Unknown");
}

// --- Call (Task 15) ---------------------------------------------------------

TEST(ExpressionTyper, CallsAUserFunction) {
    EXPECT_EQ(typed_name("f(1)", {{"f", Type::callable({Type::int_()}, Type::str())}}), "str");
}

TEST(ExpressionTyper, ReportsCallArityBothWays) {
    // Verified against real mypy 1.18.1 that a
    // callee with no recoverable parameter names gets mypy's actual
    // name-free spelling, "Too few arguments for \"f\"" -- never an invented
    // count form. Type::callable carries no parameter names, so this is the
    // truthful substitute, lower-cased to match "too many arguments for"'s
    // own convention one branch below.
    const Typed missing =
        type_expression("f()", {{"f", Type::callable({Type::int_()}, Type::str())}});
    EXPECT_EQ(only_error(missing).code, "TypeError");
    EXPECT_EQ(only_error(missing).message, "too few arguments for \"f\"");

    const Typed extra = type_expression(
        "f(1, 2)", {{"f", Type::callable({Type::int_()}, Type::str())}});
    EXPECT_EQ(only_error(extra).code, "TypeError");
    EXPECT_EQ(only_error(extra).message, "too many arguments for \"f\"");
}

// Numbered from the first USER argument; self is never counted.
TEST(ExpressionTyper, ReportsAnIncompatibleArgument) {
    const Typed typed = type_expression(
        "f(\"s\")", {{"f", Type::callable({Type::int_()}, Type::str())}});

    const diagnostics::Diagnostic error = only_error(typed);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message,
              "argument 1 to \"f\" has incompatible type \"str\"; expected \"int\"");
}

// The parameter type is the argument's expected context, which is what makes
// f([]) work when f takes a list[int].
TEST(ExpressionTyper, TheParameterTypeIsTheArgumentsContext) {
    const Typed typed = type_expression(
        "f([])", {{"f", Type::callable({Type::list_of(Type::int_())}, Type::none())}});

    EXPECT_TRUE(typed.diagnostics.empty()) << "[] takes its element type from the parameter";
}

TEST(ExpressionTyper, ConstructsAUserClass) {
    ClassTable table;
    table.declare("Widget", {});
    table.declare_method("Widget", "__init__",
                         Type::callable({Type::class_of("Widget"), Type::int_()},
                                        Type::none()));

    EXPECT_EQ(type_expression("Widget(1)", {}, Type::unknown(), &table).printed, "Widget");
}

// Verified: `class C: pass` then C(1) is "Too many arguments for C".
TEST(ExpressionTyper, AClassWithNoInitTakesNoArguments) {
    ClassTable table;
    table.declare("Widget", {});

    const Typed typed = type_expression("Widget(1)", {}, Type::unknown(), &table);
    EXPECT_EQ(only_error(typed).code, "TypeError");
}

TEST(ExpressionTyper, CallsAMethodWithSelfDropped) {
    ClassTable table;
    table.declare("Widget", {});
    table.declare_method("Widget", "area",
                         Type::callable({Type::class_of("Widget")}, Type::int_()));

    const Typed typed = type_expression("w.area()", {{"w", Type::class_of("Widget")}},
                                        Type::unknown(), &table);
    EXPECT_TRUE(typed.diagnostics.empty());
    EXPECT_EQ(typed.printed, "int");
}

TEST(ExpressionTyper, TypesTheSupportedBuiltinCalls) {
    EXPECT_EQ(typed_name("print(1)"), "None");
    EXPECT_EQ(typed_name("len([1])"), "int");
    EXPECT_EQ(typed_name("range(3)"), "range");
    EXPECT_EQ(typed_name("range(1, 10, 2)"), "range");
    EXPECT_EQ(typed_name("int(\"5\")"), "int");
    EXPECT_EQ(typed_name("str(5)"), "str");
    EXPECT_EQ(typed_name("abs(-1)"), "int");
    EXPECT_EQ(typed_name("abs(-1.5)"), "float");
    EXPECT_EQ(typed_name("sorted([1])"), "list[int]");
    EXPECT_EQ(typed_name("divmod(5, 2)"), "tuple[int, int]");
}

// An unsupported builtin is NotImplementedError, NEVER NameError -- the name
// IS defined, mypy accepts the call, and a NameError would be a false
// positive against the hard invariant.
TEST(ExpressionTyper, ReportsAnUnsupportedBuiltinCallAsUnsupportedNotUndefined) {
    const Typed typed = type_expression("zip([1], [2])");

    const diagnostics::Diagnostic error = only_error(typed);
    EXPECT_EQ(error.code, "NotImplementedError");
    EXPECT_EQ(error.message, "calls to builtin 'zip' are not supported");
}

// list() takes the same path as []: context or an error, one rule for the
// construct rather than two.
TEST(ExpressionTyper, BareContainerConstructorsFollowTheEmptyDisplayRule) {
    const Typed with_context = type_expression("list()", {}, Type::list_of(Type::int_()));
    EXPECT_TRUE(with_context.diagnostics.empty());
    EXPECT_EQ(type_name(with_context.type), "list[int]");

    // Silent, like the empty display it shares a path with: the ASSIGNMENT
    // reports the name-bearing var-annotated message (Task 17), not the call.
    const Typed without = type_expression("list()");
    EXPECT_TRUE(without.diagnostics.empty());
    EXPECT_EQ(without.printed, "Unknown");
}

// A nullopt from builtin_call_result for
// a SUPPORTED name means "this shape is not modelled", never "mypy rejects
// this" -- so it must be NotImplementedError, not a false TypeError.
// `list(range(3))` is mypy-clean and about as common as Python gets;
// `round(x, 2)` is a mypy-clean two-argument overload this table does not
// model; `len(w)` on a user class defining `__len__` is mypy-clean but
// invisible to this model, which has no typeshed. All three used to draw a
// false TypeError.
TEST(ExpressionTyper, ReportsAnUnmodelledSupportedBuiltinShapeAsUnsupportedNotAFalseTypeError) {
    const Typed list_call = type_expression("list(range(3))");
    const diagnostics::Diagnostic list_error = only_error(list_call);
    EXPECT_EQ(list_error.code, "NotImplementedError");
    EXPECT_EQ(list_error.message,
              "calls to builtin 'list' with these argument types are not supported");

    const Typed round_call = type_expression("round(1.5, 2)");
    EXPECT_EQ(only_error(round_call).code, "NotImplementedError");

    ClassTable table;
    table.declare("Widget", {});
    table.declare_method("Widget", "__len__",
                         Type::callable({Type::class_of("Widget")}, Type::int_()));
    const Typed len_call =
        type_expression("len(w)", {{"w", Type::class_of("Widget")}}, Type::unknown(), &table);
    EXPECT_EQ(only_error(len_call).code, "NotImplementedError");
}

TEST(ExpressionTyper, ReportsCallingANonCallable) {
    const Typed typed = type_expression("x()", {{"x", Type::int_()}});

    const diagnostics::Diagnostic error = only_error(typed);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "\"int\" not callable");
}

// Verified: `c(1)` with __call__ defined is mypy-CLEAN, so a TypeError on a
// Class receiver would be false.
TEST(ExpressionTyper, CallingAClassInstanceIsUnsupportedNotAnError) {
    ClassTable table;
    table.declare("Widget", {});

    const Typed typed = type_expression("w(1)", {{"w", Type::class_of("Widget")}},
                                        Type::unknown(), &table);
    EXPECT_EQ(only_error(typed).code, "NotImplementedError");
}

TEST(ExpressionTyper, ACallWithAFailedArgumentReportsOnce) {
    const Typed typed =
        type_expression("f(nope)", {{"f", Type::callable({Type::int_()}, Type::str())}});

    EXPECT_EQ(typed.diagnostics.size(), 1u) << "the NameError is the only root cause";
    EXPECT_EQ(typed.diagnostics.front().code, "NameError");
    EXPECT_EQ(typed.printed, "str") << "the call's return type is still known";
}

// The class-object counterpart of CallsAMethodWithSelfDropped: `Widget.resize`
// keeps self (see ResolvesAClassObjectMethodReferenceKeepingSelf), so calling
// it through the class object must supply self EXPLICITLY as the first
// argument -- mypy accepts `C.m(c)` for exactly this reason. This is the test
// that would catch a double-drop of args[0] in the Call arm.
TEST(ExpressionTyper, CallingThroughTheClassObjectRequiresSelfExplicitly) {
    ClassTable table;
    table.declare("Widget", {});
    table.declare_method("Widget", "resize",
                         Type::callable({Type::class_of("Widget"), Type::int_()}, Type::none()));

    const Typed typed = type_expression("Widget.resize(w, 1)", {{"w", Type::class_of("Widget")}},
                                        Type::unknown(), &table);
    EXPECT_TRUE(typed.diagnostics.empty())
        << "self is still required positionally through the class object";
    EXPECT_EQ(typed.printed, "None");
}

// A named diagnostic naming the METHOD and its CLASS, not a bare function
// name -- `"m" of "C"`, verified against real mypy 1.18.1.
TEST(ExpressionTyper, ReportsAMethodArgumentErrorNamingTheMethodAndItsClass) {
    ClassTable table;
    table.declare("Widget", {});
    table.declare_method("Widget", "resize",
                         Type::callable({Type::class_of("Widget"), Type::int_()}, Type::none()));

    const Typed typed = type_expression("w.resize(\"s\")", {{"w", Type::class_of("Widget")}},
                                        Type::unknown(), &table);
    const diagnostics::Diagnostic error = only_error(typed);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message,
              "argument 1 to \"resize\" of \"Widget\" has incompatible type \"str\"; expected "
              "\"int\"");
}

// A Union callee needs narrowing before mypy would even decide whether the
// call is legal (e.g. `Callable[[], int] | None`); reporting a plain
// TypeError here would risk a false positive on a union whose every member
// is in fact callable, so this must match every other operand arm's Union
// handling (Attribute, BinOp, ...): NotImplementedError, never TypeError.
TEST(ExpressionTyper, ReportsCallingAUnionAsNeedingNarrowing) {
    const Typed typed = type_expression(
        "x()", {{"x", Type::union_of({Type::callable({}, Type::int_()), Type::none()})}});

    const diagnostics::Diagnostic error = only_error(typed);
    EXPECT_EQ(error.code, "NotImplementedError");
    EXPECT_EQ(error.message,
              "operations on a union-typed value require narrowing, which is not supported");
}

// ListComp (Task 16), the only expression that pushes a scope. Verified
// against mypy 1.18.1: iterating a dict yields its keys.
TEST(ExpressionTyper, TypesAListComprehension) {
    EXPECT_EQ(typed_name("[i for i in [1, 2]]"), "list[int]");
    EXPECT_EQ(typed_name("[s for s in \"abc\"]"), "list[str]");
    EXPECT_EQ(typed_name("[k for k in d]", {{"d", Type::dict_of(Type::str(), Type::int_())}}),
             "list[str]") << "iterating a dict yields its keys";
}

// Corpus defect 1 (2026-09-07): a list comprehension's element expression
// reading its OWN loop variable (`[v * v for v in values]`, nearly every real
// comprehension) is mypy-clean but drew a false `NameError: name 'v' is used
// before definition` -- TWICE, once per occurrence of `v` -- because the
// comprehension target's Binding never set order_exempt. This is the THIRD
// site needing that flag (see Binding::order_exempt's own comment: function
// parameters were the first, a `for` target the second).
//
// type_expression()/typed_name() cannot exercise this: the shared harness
// never calls ExpressionTyper::set_statement_line, so statement_line_ stays
// at its inert default (std::numeric_limits<int>::max()) and the ordering
// check this bug lives in can never fire -- exactly why
// TypesAListComprehension above, whose element is a bare `i`, passed even
// with the bug present, and exactly why no existing unit test caught this
// before the corpus did. This test builds the pipeline by hand instead, so
// it can call set_statement_line(1) itself, mirroring what TypeChecker does
// for every real statement. `values` is bound at declared_line=0 -- an
// earlier, real line -- rather than through the bindings map (which pins
// declared_line=1, indistinguishable from a same-line binding for this
// check), so the test isolates the comprehension target's own exemption
// rather than accidentally tripping the ordering check on `values` too.
TEST(ExpressionTyper, AListComprehensionMayReadItsOwnTarget) {
    const std::string source = "[v * v for v in values]\n";
    lexer::Lexer lexer(source);
    const lexer::TokenStream lexed(lexer.tokenize());

    diagnostics::DiagnosticSink parse_sink;
    lexer::TokenStream tokens = lexer::IndentationPass().run(lexed, parse_sink);
    const std::unique_ptr<ast::Module> module =
        parser::StatementParser(tokens, parse_sink).parse_module();
    ASSERT_TRUE(parse_sink.empty()) << "fixture must parse cleanly";
    ASSERT_EQ(module->body().size(), 1u) << "fixture must be one statement";

    const auto* statement = dynamic_cast<const ast::ExprStmt*>(module->body().front().get());
    ASSERT_NE(statement, nullptr) << "fixture must be an expression statement";

    ScopeStack scopes;
    Binding values_binding;
    values_binding.type = Type::list_of(Type::int_());
    values_binding.declared_line = 0;
    scopes.bind("values", values_binding);

    ClassTable classes;
    TypeMap types;
    NarrowingMap narrowings;
    diagnostics::DiagnosticSink sink;
    ExpressionTyper typer(scopes, classes, types, narrowings, sink);
    typer.set_statement_line(1);

    const Type result = typer.type_of(statement->value(), Type::unknown());

    EXPECT_TRUE(sink.diagnostics().empty())
        << "expected no diagnostics, got "
        << (sink.diagnostics().empty() ? "" : sink.diagnostics().front().message);
    EXPECT_EQ(type_name(result), "list[int]");
}

// Verified against mypy 1.18.1: `xs = [i for i in [1, 2]]` then `print(i)`
// reports `Name "i" is not defined`. Each type_expression() call below gets
// its OWN fresh ScopeStack, so this alone cannot distinguish a real pop from
// a leak -- AFailedClauseStillPopsTheComprehensionScope below is the test
// that pins the RAII guard within a single ScopeStack.
TEST(ExpressionTyper, TheComprehensionTargetDoesNotLeak) {
    const Typed typed = type_expression("[i for i in [1]]");
    EXPECT_TRUE(typed.diagnostics.empty());

    // A second expression referring to i must not resolve.
    const Typed after = type_expression("i");
    EXPECT_EQ(only_error(after).code, "NameError");
}

TEST(ExpressionTyper, AComprehensionReadsOutward) {
    EXPECT_EQ(typed_name("[g for i in [1]]", {{"g", Type::str()}}), "list[str]");
}

TEST(ExpressionTyper, ComprehensionConditionsAreChecked) {
    const Typed typed = type_expression("[i for i in [1] if nope]");

    EXPECT_EQ(only_error(typed).code, "NameError");
}

TEST(ExpressionTyper, ALaterClauseSeesAnEarlierClausesTarget) {
    EXPECT_EQ(typed_name("[b for a in [[1]] for b in a]"), "list[int]");
}

TEST(ExpressionTyper, ReportsIteratingANonIterable) {
    const Typed typed = type_expression("[i for i in 1]");

    const diagnostics::Diagnostic error = only_error(typed);
    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "\"int\" is not iterable");
}

// mypy ACCEPTS tuple targets, so this must not be a TypeError. Binding
// element-wise is wrong because element_type of a tuple[K, V] is the union
// K | V, not a positional pair.
TEST(ExpressionTyper, ReportsATupleTargetAsUnsupported) {
    const Typed typed = type_expression("[k for k, v in [(1, \"a\")]]");

    const diagnostics::Diagnostic error = only_error(typed);
    EXPECT_EQ(error.code, "NotImplementedError");
    EXPECT_EQ(error.message, "tuple targets in comprehensions are not supported");
}

// Pins the RAII guard specifically: `a` (clause 0's own target, bound int
// from [1]) is a "bad name" as clause 1's iterable -- typing it succeeds
// (it resolves fine), but its element_type is NotApplicable (int is not
// iterable), so the arm takes an early return AFTER the guard has already
// pushed the comprehension scope on clause 0. Both halves of this tuple are
// typed by the SAME ExpressionTyper/ScopeStack instance (one type_expression
// call, one statement), unlike TheComprehensionTargetDoesNotLeak above, so a
// guard that failed to pop on this early return would leave `a` still bound
// when the tuple's second element is typed -- turning the expected NameError
// below into a silent, wrong resolution to int.
TEST(ExpressionTyper, AFailedClauseStillPopsTheComprehensionScope) {
    const Typed typed = type_expression("([b for a in [1] for b in a], a)");

    ASSERT_EQ(typed.diagnostics.size(), 2u)
        << "one TypeError for the non-iterable clause, one NameError for `a` "
           "read back outside the (properly popped) comprehension scope";
    EXPECT_EQ(typed.diagnostics[0].code, "TypeError");
    EXPECT_EQ(typed.diagnostics[0].message, "\"int\" is not iterable");
    EXPECT_EQ(typed.diagnostics[1].code, "NameError");
    EXPECT_EQ(typed.diagnostics[1].message, "name 'a' is not defined");
}

} // namespace
} // namespace cythonpp::domain::semantic
