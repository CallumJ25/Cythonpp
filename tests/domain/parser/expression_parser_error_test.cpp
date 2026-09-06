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

TEST(ExpressionParserError, AnUnclosedParenthesisIsReportedAgainstItsOpener) {
    // Reported at the '(' on line 1, not at end of input, so the message
    // points at the line that needs fixing.
    expect_error("(a + b", "'(' was never closed", 1, 1);
}

TEST(ExpressionParserError, AnUnclosedParenthesisSpanningLinesNamesTheOpeningLine) {
    // The lexer suppresses the newline inside brackets, so this is one
    // logical line and the report must still name line 1.
    expect_error("(a,\n b", "'(' was never closed", 1, 1);
}

TEST(ExpressionParserError, AMismatchedCloserNamesBothBrackets) {
    expect_error("(a]", "closing ']' does not match '(' opened on line 1", 1, 3);
}

TEST(ExpressionParserError, AnEmptyGroupWithNoCloserIsReported) {
    expect_error("(", "'(' was never closed", 1, 1);
}

TEST(ExpressionParserError, AParenthesisedAssignmentExpressionIsRejected) {
    // Deferred here from Task 8: '(' only became an atom in this task. This
    // is the spelling that motivated the check in parse_expression at all --
    // without it the paren rule below would report "'(' was never closed"
    // and point at column 1 instead of at the ':='.
    expect_error("(n := 1)", "assignment expressions are not supported", 1, 4);
}

TEST(ExpressionParserError, SlicesAreRejected) {
    expect_error("items[1:2]", "slices are not supported", 1, 8);
    expect_error("items[:2]", "slices are not supported", 1, 7);
    expect_error("items[1:]", "slices are not supported", 1, 8);
}

TEST(ExpressionParserError, KeywordArgumentsAreRejected) {
    expect_error("f(k=1)", "keyword arguments are not supported", 1, 4);
}

TEST(ExpressionParserError, GeneratorExpressionsAreRejected) {
    expect_error("f(x for x in y)", "generator expressions are not supported", 1, 5);
}

TEST(ExpressionParserError, StarredArgumentsAreRejected) {
    expect_error("f(*a)", "starred expressions are not supported", 1, 3);
    expect_error("f(**a)", "starred expressions are not supported", 1, 3);
}

TEST(ExpressionParserError, AMissingAttributeNameIsReported) {
    // The brief's literal fixture is "a.1", but the lexer's scan_number rule
    // (lexer.cpp:158, `c == '.' && is_digit(peek(1))`) folds a dot directly
    // followed by a digit into a float literal -- confirmed by dumping the
    // token stream for "a.1": IDENTIFIER "a", LITERAL_FLOAT ".1", with no DOT
    // token at all. parse_postfix's loop then never sees a DOT, so the whole
    // expression parses as bare `a` with a dangling ".1" left unconsumed,
    // and this test never reaches parse_attribute's error branch. "a.+"
    // reproduces the same shape (DOT immediately followed by a non-identifier
    // token) without tripping the number scanner, landing on the same column.
    expect_error("a.+", "expected an attribute name after '.'", 1, 3);
}

TEST(ExpressionParserError, AnUnclosedBracketIsReportedAgainstItsOpener) {
    expect_error("items[0", "'[' was never closed", 1, 6);
    expect_error("f(a", "'(' was never closed", 1, 2);
}

TEST(ExpressionParserError, NestedUnclosedBracketsNameTheInnermostOpener) {
    // Deferred here from Task 10, which could only nest via a subscript
    // trailer because '[' was not yet an atom. This is the real list-literal
    // nesting. CPython names the innermost opener too, and it is the one the
    // innermost production's local naturally holds.
    expect_error("f(a, [b, c", "'[' was never closed", 1, 6);
}

TEST(ExpressionParserError, ANonAssignableComprehensionTargetIsReported) {
    expect_error("[x for 1 in y]", "cannot assign to literal", 1, 8);
    expect_error("[x for f() in y]", "cannot assign to function call", 1, 8);
}

TEST(ExpressionParserError, AComprehensionWithoutInIsReported) {
    expect_error("[x for y z]", "expected 'in' after a comprehension target", 1, 10);
}

TEST(ExpressionParserError, AnUnclosedListIsReportedAgainstItsOpener) {
    expect_error("[1, 2", "'[' was never closed", 1, 1);
    expect_error("[x for x in y", "'[' was never closed", 1, 1);
    // A bare opener, and one cut off right after a comma. Both hit the
    // ends_a_sequence guards rather than reporting "expected an expression".
    expect_error("[", "'[' was never closed", 1, 1);
    expect_error("[1,", "'[' was never closed", 1, 1);
}

TEST(ExpressionParserError, SetDisplaysAreRejected) {
    expect_error("{1, 2}", "set displays are not supported", 1, 3);
}

TEST(ExpressionParserError, SetComprehensionsAreRejected) {
    expect_error("{x for x in y}", "set comprehensions are not supported", 1, 4);
}

TEST(ExpressionParserError, DictComprehensionsAreRejected) {
    expect_error("{k: v for k, v in pairs}", "dict comprehensions are not supported", 1, 7);
}

TEST(ExpressionParserError, AMissingColonInADictIsReported) {
    expect_error("{'a': 1, 'b'}", "expected ':' in a dict display", 1, 13);
}

TEST(ExpressionParserError, AnUnclosedBraceIsReportedAgainstItsOpener) {
    expect_error("{'a': 1", "'{' was never closed", 1, 1);
    expect_error("{", "'{' was never closed", 1, 1);
    expect_error("{'a': 1,", "'{' was never closed", 1, 1);
}

TEST(ExpressionParserError, NestedUnclosedSubscriptNamesTheInnermostOpener) {
    // The subscript-trailer spelling of nesting. Task 10 used this fixture as
    // a stand-in while '[' was not yet an atom; Task 11's list-literal version
    // then took over the original test name, so this path lost its coverage to
    // a rename. Restored under a distinct name -- it exercises
    // parse_subscript's unclosed path, not parse_bracket_atom's.
    expect_error("f(a, b[c, d", "'[' was never closed", 1, 7);
}

TEST(ExpressionParserError, AnEmptyTupleIsNotAnAssignableTarget) {
    // is_assignable rejects an empty TupleExpr explicitly; without that check
    // `for () in y` would be accepted as a valid target. The branch existed
    // from Task 11 but nothing exercised it.
    expect_error("[x for () in y]", "cannot assign to this expression", 1, 8);
}

} // namespace
} // namespace cythonpp::domain::parser
