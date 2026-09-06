#include <gtest/gtest.h>

#include <string>

#include "parse_test_helpers.h"

namespace cythonpp::domain::parser {
namespace {

using test_support::only_error;
using test_support::parse;

void expect_error(const std::string& source, const std::string& message, int line, int column) {
    const diagnostics::Diagnostic diagnostic = only_error(parse(source));
    EXPECT_EQ(diagnostic.code, "SyntaxError") << source;
    EXPECT_EQ(diagnostic.message, message) << source;
    EXPECT_EQ(diagnostic.line, line) << source;
    EXPECT_EQ(diagnostic.column, column) << source;
    EXPECT_EQ(diagnostic.severity, diagnostics::Severity::Error) << source;
}

TEST(ExpressionParserError, EmptyInputIsNotAnExpression) {
    expect_error("", "expected an expression", 1, 1);
}

TEST(ExpressionParserError, FStringsAreRejectedBeforeTheLiteralRule) {
    // f-string parts carry OBJECT, so without an explicit test first they
    // would build a Constant out of a fragment.
    expect_error("f'{x}'", "f-strings are not supported", 1, 1);
}

TEST(ExpressionParserError, UnsupportedOperandConstructsNameThemselves) {
    expect_error("lambda: 1", "lambda expressions are not supported", 1, 1);
    expect_error("await x", "await expressions are not supported", 1, 1);
    expect_error("yield x", "yield expressions are not supported", 1, 1);
}

TEST(ExpressionParserError, ImplicitStringConcatenationIsRejected) {
    // The only silent failure in the set: without a rule, this parses as "a"
    // and drops the rest, so the program compiles and means something else.
    expect_error("'a' 'b'", "implicit string concatenation is not supported", 1, 5);
}

TEST(ExpressionParserError, AFailedParseReportsExactlyOneDiagnostic) {
    const test_support::ParseResult result = parse("lambda: 1");
    EXPECT_FALSE(result.succeeded());
    EXPECT_EQ(result.diagnostics.size(), 1u);
}

TEST(ExpressionParserError, AMissingOperandAfterAPrefixOperatorIsReported) {
    expect_error("-", "expected an expression", 1, 2);
}

TEST(ExpressionParserError, AMissingExponentIsReported) {
    expect_error("2 **", "expected an expression", 1, 5);
}

TEST(ExpressionParserError, AMissingRightOperandIsReported) {
    expect_error("1 +", "expected an expression", 1, 4);
}

TEST(ExpressionParserError, NotWithoutInAfterAnOperandIsReported) {
    expect_error("a not b", "expected 'in' after 'not'", 1, 7);
}

TEST(ExpressionParserError, AMissingComparisonOperandIsReported) {
    expect_error("a <", "expected an expression", 1, 4);
}

TEST(ExpressionParserError, ConditionalExpressionsAreRejected) {
    // There is no IfExp node. Rejected in parse_expression rather than in an
    // atom rule, because a comprehension's own `if` is parsed by
    // parse_or_test and never reaches this check.
    expect_error("a if c else b", "conditional expressions are not supported", 1, 3);
}

TEST(ExpressionParserError, AMissingOperandAfterAndIsReported) {
    expect_error("a and", "expected an expression", 1, 6);
}

TEST(ExpressionParserError, AssignmentExpressionsAreRejectedAfterTheirTarget) {
    // `n` fills the operand slot, so ':=' is never seen at atom position.
    // Without this check the enclosing paren rule reports "'(' was never
    // closed", which points at the wrong thing entirely.
    //
    // The brief's parenthesized case, `(n := 1)`, is deferred: parens are
    // not parsed until Task 9's parse_paren_atom exists. Until then `(`
    // itself fails in parse_atom with "expected an expression" at column 1,
    // before this check ever runs -- confirmed by running this test with
    // that line included, which fails exactly that way. Re-add
    // `expect_error("(n := 1)", "assignment expressions are not supported",
    // 1, 4);` once Task 9 lands.
    expect_error("n := 1", "assignment expressions are not supported", 1, 3);
}

TEST(ExpressionParserError, AWalrusInOperandPositionIsAlsoRejected) {
    // The atom-rule branch, reached only when ':=' starts an operand.
    expect_error(":= 1", "assignment expressions are not supported", 1, 1);
}

} // namespace
} // namespace cythonpp::domain::parser
