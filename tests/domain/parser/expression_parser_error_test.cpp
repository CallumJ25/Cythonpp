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

} // namespace
} // namespace cythonpp::domain::parser
