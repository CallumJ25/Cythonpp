#include <memory>
#include <optional>
#include <string>

#include <gtest/gtest.h>

#include "domain/ast/expr_stmt.h"
#include "domain/codegen/emitter.h"

#include "emitter_fixture.h"

namespace cythonpp::domain::codegen {
namespace {

// Emits the expression of a single trailing ExprStmt. `source` must end with
// one, e.g. "x: int = 1\nx\n".
std::optional<std::string> emit_last_expression(Fixture& fixture) {
    const ast::Stmt& last = *fixture.module->body().back();
    const auto* statement = dynamic_cast<const ast::ExprStmt*>(&last);
    EXPECT_NE(statement, nullptr) << "fixture must end in an expression statement";
    Emitter emitter(fixture.types, fixture.emit_sink);
    return emitter.emit_expression_for_test(statement->value());
}

std::optional<std::string> emitted(const std::string& source) {
    Fixture fixture = build(source);
    return emit_last_expression(fixture);
}

TEST(Emitter, IntegerLiteral) {
    EXPECT_EQ(emitted("5\n").value(), "py::int_(5)");
    EXPECT_EQ(emitted("1_000\n").value(), "py::int_(1000)");
}

TEST(Emitter, FloatLiteral) {
    EXPECT_EQ(emitted("1.5\n").value(), "py::float_(1.5)");
}

TEST(Emitter, BoolAndNoneLiterals) {
    EXPECT_EQ(emitted("True\n").value(), "py::bool_(true)");
    EXPECT_EQ(emitted("False\n").value(), "py::bool_(false)");
    EXPECT_EQ(emitted("None\n").value(), "py::none");
}

// Bytes are emitted with three-digit OCTAL escapes, never \\x: a hex escape in
// C++ is greedy, so "\\x41" followed by a literal 'B' would be read as one
// escape "\\x41B". Octal is exactly three digits and cannot run on.
TEST(Emitter, StringLiteralUsesNonGreedyOctalEscapes) {
    EXPECT_EQ(emitted("\"hi\"\n").value(), "py::str(std::string(\"\\150\\151\", 2))");
    EXPECT_EQ(emitted("\"\"\n").value(), "py::str(std::string(\"\", 0))");
}

TEST(Emitter, StringEscapesAreDecodedBeforeReEncoding) {
    // Python "a\\nb" is three bytes: 'a', newline, 'b'.
    EXPECT_EQ(emitted("\"a\\nb\"\n").value(), "py::str(std::string(\"\\141\\012\\142\", 3))");
}

// Lexer::scan_string does not emit an error token for an unterminated string:
// it just stops at the next unescaped newline, so source `"a\"` + newline (a
// backslash immediately before what should be the closing quote) tokenizes
// as a complete LITERAL_STRING whose lexeme is the 4 bytes `"`, `a`, `\`,
// `"` -- the lexeme's LAST character happens to equal the quote character,
// but it was consumed as the escape's target, not left as a real terminator.
// Decoding it anyway would silently produce the wrong, shorter string
// {'a', '"'}; this must be refused instead.
TEST(Emitter, AStringLiteralWhoseTrailingBackslashConsumesTheClosingQuoteIsRefused) {
    Fixture fixture = build("\"a\\\"\n");
    const std::optional<std::string> text = emit_last_expression(fixture);

    EXPECT_FALSE(text.has_value());
    ASSERT_EQ(fixture.emit_sink.diagnostics().size(), 1U);
    EXPECT_EQ(fixture.emit_sink.diagnostics().front().code, "NotImplementedError");
}

TEST(Emitter, NameIsMangled) {
    EXPECT_EQ(emitted("x: int = 1\nx\n").value(), "cy_x");
}

// Refusals: each reports exactly one diagnostic and yields nullopt.
TEST(Emitter, AnUnsupportedConstructIsRefusedNotEmitted) {
    Fixture fixture = build("xs: list[int] = [1]\nxs\n");
    const std::optional<std::string> text = emit_last_expression(fixture);

    EXPECT_FALSE(text.has_value());
    ASSERT_EQ(fixture.emit_sink.diagnostics().size(), 1U);
    EXPECT_EQ(fixture.emit_sink.diagnostics().front().code, "NotImplementedError");
}

TEST(Emitter, ANonDecimalIntegerLiteralIsRefused) {
    Fixture fixture = build("0x10\n");
    EXPECT_FALSE(emit_last_expression(fixture).has_value());
    EXPECT_FALSE(fixture.emit_sink.empty());
}

TEST(Emitter, ANonAsciiIdentifierIsRefused) {
    Fixture fixture = build("caf\xc3\xa9: int = 1\ncaf\xc3\xa9\n");
    EXPECT_FALSE(emit_last_expression(fixture).has_value());
    EXPECT_FALSE(fixture.emit_sink.empty());
}

TEST(Emitter, ArithmeticEmitsRuntimeCallsNotOperators) {
    EXPECT_EQ(emitted("1 + 2\n").value(), "py::add(py::int_(1), py::int_(2))");
    EXPECT_EQ(emitted("1 - 2\n").value(), "py::sub(py::int_(1), py::int_(2))");
    EXPECT_EQ(emitted("1 * 2\n").value(), "py::mul(py::int_(1), py::int_(2))");
    EXPECT_EQ(emitted("1 / 2\n").value(), "py::truediv(py::int_(1), py::int_(2))");
    EXPECT_EQ(emitted("1 // 2\n").value(), "py::floordiv(py::int_(1), py::int_(2))");
    EXPECT_EQ(emitted("1 % 2\n").value(), "py::mod(py::int_(1), py::int_(2))");
}

// Nesting composes with no parenthesisation logic, because every operation is
// a CALL. This is the property that makes precedence a non-problem.
TEST(Emitter, NestedArithmeticNeedsNoPrecedenceHandling) {
    EXPECT_EQ(emitted("1 + 2 * 3\n").value(),
              "py::add(py::int_(1), py::mul(py::int_(2), py::int_(3)))");
}

TEST(Emitter, UnaryOperators) {
    EXPECT_EQ(emitted("-1\n").value(), "py::neg(py::int_(1))");
    EXPECT_EQ(emitted("+1\n").value(), "py::int_(1)");
    EXPECT_EQ(emitted("not True\n").value(), "py::not_(py::bool_(true))");
}

// Python's bool is an int subtype, so True + 1 is 2. The widening is inserted
// by the emitter because the runtime has no bool arithmetic overloads.
TEST(Emitter, BoolOperandsAreWidenedToInt) {
    EXPECT_EQ(emitted("True + 1\n").value(),
              "py::add(py::to_int(py::bool_(true)), py::int_(1))");
}

// --- Decision 4a: the power gate -------------------------------------------

TEST(Emitter, PowerWithANonNegativeIntegerLiteralExponentIsEmitted) {
    EXPECT_EQ(emitted("2 ** 10\n").value(), "py::pow(py::int_(2), py::int_(10))");
    EXPECT_EQ(emitted("2 ** 0\n").value(), "py::pow(py::int_(2), py::int_(0))");
    EXPECT_EQ(emitted("2.0 ** 3\n").value(), "py::pow(py::float_(2.0), py::int_(3))");
}

// THE GATE MUST BE ON AST SHAPE, NOT TYPE. A negative literal parses as
// UnaryOp(-, Constant), and the TypeMap types this whole expression as `int`
// -- which is WRONG, the value is 0.5. A gate that consulted the type would
// happily emit an int-typed expression here and print 0 instead of 0.5.
TEST(Emitter, PowerWithANegativeLiteralExponentIsRefused) {
    Fixture fixture = build("2 ** -1\n");
    EXPECT_FALSE(emit_last_expression(fixture).has_value());
    ASSERT_FALSE(fixture.emit_sink.empty());
    EXPECT_EQ(fixture.emit_sink.diagnostics().front().code, "NotImplementedError");
}

// mypy itself types a non-literal exponent as Any -- it gives up -- so there
// is no honest C++ type to emit.
TEST(Emitter, PowerWithANonLiteralExponentIsRefused) {
    Fixture fixture = build("a: int = 2\nb: int = 3\na ** b\n");
    EXPECT_FALSE(emit_last_expression(fixture).has_value());
    EXPECT_FALSE(fixture.emit_sink.empty());
}

// (-8.0) ** 0.5 is COMPLEX in both CPython and mypy, and float in cythonpp's
// TypeMap. Refusing every non-integer exponent covers it without needing to
// know the base's sign, which nothing here does.
TEST(Emitter, PowerWithAFractionalExponentIsRefused) {
    Fixture fixture = build("(-8.0) ** 0.5\n");
    EXPECT_FALSE(emit_last_expression(fixture).has_value());
    EXPECT_FALSE(fixture.emit_sink.empty());
}

// --- Comparison and boolean operators ---------------------------------------

TEST(Emitter, SimpleComparison) {
    EXPECT_EQ(emitted("1 < 2\n").value(),
              "py::lt(py::int_(1), py::int_(2))");
}

// The middle term must be evaluated EXACTLY ONCE, which is why a chain emits
// an immediately-invoked lambda binding each operand to a temporary rather
// than desugaring to `a < b && b < c`.
TEST(Emitter, ComparisonChainEvaluatesEachOperandOnce) {
    const std::string text = emitted("1 < 2 < 3\n").value();

    EXPECT_NE(text.find("[&]() -> py::bool_"), std::string::npos);
    EXPECT_NE(text.find("auto&& _cy_cmp_1 = (py::int_(2));"), std::string::npos)
        << "the middle operand must be bound to one temporary";
    EXPECT_EQ(text.find("py::int_(2)", text.find("py::int_(2)") + 1), std::string::npos)
        << "the middle operand must appear exactly once in the emitted text";
}

// and/or return an OPERAND, not a bool, and short-circuit. The right side is
// passed as a lambda so it is evaluated at most once and only if needed.
TEST(Emitter, BooleanOperatorsPassTheRightSideLazily) {
    EXPECT_EQ(emitted("a: int = 1\nb: int = 2\na and b\n").value(),
              "py::and_(cy_a, [&]{ return cy_b; })");
    EXPECT_EQ(emitted("a: int = 1\nb: int = 2\na or b\n").value(),
              "py::or_(cy_a, [&]{ return cy_b; })");
}

// Differing operand types give the expression a Union type, which
// cpp_type_name maps to nullopt. The refusal falls out of that with no rule
// of its own -- see Task 4.
TEST(Emitter, BooleanOperatorsOverDifferingTypesAreRefused) {
    Fixture fixture = build("a: int = 1\nb: str = \"x\"\na and b\n");
    EXPECT_FALSE(emit_last_expression(fixture).has_value());
    EXPECT_FALSE(fixture.emit_sink.empty());
}

// --- Calls ------------------------------------------------------------------

TEST(Emitter, PrintAndLen) {
    EXPECT_EQ(emitted("print(1)\n").value(), "py::print(py::int_(1))");
    EXPECT_EQ(emitted("print(1, 2)\n").value(), "py::print(py::int_(1), py::int_(2))");
    EXPECT_EQ(emitted("print()\n").value(), "py::print()");
    EXPECT_EQ(emitted("len(\"ab\")\n").value(),
              "py::len(py::str(std::string(\"\\141\\142\", 2)))");
}

TEST(Emitter, ACallToADefinedFunctionIsMangled) {
    EXPECT_EQ(emitted("def f(a: int) -> int:\n    return a\n\nf(1)\n").value(),
              "cy_f(py::int_(1))");
}

TEST(Emitter, AnUnsupportedBuiltinCallIsRefused) {
    Fixture fixture = build("abs(-1)\n");
    EXPECT_FALSE(emit_last_expression(fixture).has_value());
    EXPECT_FALSE(fixture.emit_sink.empty());
}

// --- Review round 1 fixes ---------------------------------------------------

// Python's bool is an int subtype, so `True ** 2` is `1`. cpp_type_name(Bool)
// is a valid mapping ("py::bool_"), so emit_power's base-representability
// check alone let this through with no widening: py::pow has exactly two
// overloads, pow(int_, int_) and pow(float_, int_), and bool_'s constructor
// is explicit with no conversion operator, so the unwidened form does not
// compile.
TEST(Emitter, PowerWidensABoolBaseToInt) {
    EXPECT_EQ(emitted("True ** 2\n").value(),
              "py::pow(py::to_int(py::bool_(true)), py::int_(2))");
}

// str % anything is printf-style formatting: operator_rules.cpp's
// modulo_result types it as Str (a genuine, mypy-agreeing judgement), so the
// generic result-representability check let it through, but py::mod has no
// overload accepting a str at all. Printf-style formatting is genuinely
// outside this slice, so this refuses rather than emitting uncompilable code.
TEST(Emitter, ModuloOnAStrLeftOperandIsRefused) {
    Fixture fixture = build("\"x\" % 1\n");
    EXPECT_FALSE(emit_last_expression(fixture).has_value());
    EXPECT_FALSE(fixture.emit_sink.empty());
}

// A gap found during the round-1 sweep, beyond the two originally reported:
// == and != are TOTAL in operator_rules.h ("any operands, always bool"), so
// `"x" == 1` type-checks clean, but py::eq is a template over `.raw()` and
// std::string has no operator== against int64_t -- no matching
// instantiation exists.
TEST(Emitter, EqualityBetweenStrAndNumericOperandsIsRefused) {
    Fixture fixture = build("\"x\" == 1\n");
    EXPECT_FALSE(emit_last_expression(fixture).has_value());
    EXPECT_FALSE(fixture.emit_sink.empty());
}

// Same family: py::none_t has no raw() at all, so a comparison naming `None`
// on either side of == has no matching py::eq instantiation, even though
// operator_rules.h's total EQUAL/NOT_EQUAL case type-checks it clean.
TEST(Emitter, EqualityInvolvingNoneIsRefused) {
    Fixture fixture = build("1 == None\n");
    EXPECT_FALSE(emit_last_expression(fixture).has_value());
    EXPECT_FALSE(fixture.emit_sink.empty());
}

// Control: the numeric tower's raw types (bool, int64_t, double) freely
// convert between each other, so a mixed-numeric-kind equality must NOT be
// refused by the new comparability check.
TEST(Emitter, EqualityAcrossTheNumericTowerStaysClean) {
    EXPECT_EQ(emitted("True == 1\n").value(), "py::eq(py::bool_(true), py::int_(1))");
}

// FINAL-REVIEW CRITICAL 2: a call argument is widened against the PARAMETER's
// declared type, exactly as an assignment initializer and a `return` value
// already were. `bool <: int <: float` is real subtyping mypy enforces, so
// every call below is mypy-clean and CPython-clean -- but py::bool_ has no
// implicit conversion to py::int_, so the bare argument this used to emit was
// `no matching function for call to 'cy_f'`.
TEST(Emitter, CallArgumentsAreWidenedAgainstTheParameterType) {
    EXPECT_EQ(emitted("def f(x: int) -> int:\n    return x\n\n\nf(True)\n").value(),
              "cy_f(py::to_int(py::bool_(true)))");
    EXPECT_EQ(emitted("def f(x: float) -> float:\n    return x\n\n\nf(1)\n").value(),
              "cy_f(py::to_float(py::int_(1)))");
    // Two parameters, widened INDEPENDENTLY and positionally -- a single
    // shared decision would get one of these wrong.
    EXPECT_EQ(
        emitted("def f(a: float, b: int) -> int:\n    return b\n\n\nf(1, True)\n").value(),
        "cy_f(py::to_float(py::int_(1)), py::to_int(py::bool_(true)))");
}

// Control: an argument whose type already matches its parameter gets no
// wrapper at all, and a non-numeric parameter is untouched. Without this, a
// "widen everything" implementation would pass the test above.
TEST(Emitter, CallArgumentsNeedingNoWideningAreEmittedBare) {
    EXPECT_EQ(emitted("def f(x: int) -> int:\n    return x\n\n\nf(1)\n").value(),
              "cy_f(py::int_(1))");
    EXPECT_EQ(emitted("def f(s: str) -> str:\n    return s\n\n\nf(\"a\")\n").value(),
              "cy_f(py::str(std::string(\"\\141\", 1)))");
    // A BUILTIN takes no widening: py::print has one overload per printable
    // type, so a bool argument must stay a py::bool_ and print as `True`.
    EXPECT_EQ(emitted("print(True)\n").value(), "py::print(py::bool_(true))");
}

} // namespace
} // namespace cythonpp::domain::codegen
