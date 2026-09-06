#include <gtest/gtest.h>

#include <string>

#include "statement_parse_test_helpers.h"

namespace cythonpp::domain::parser {
namespace {

using statement_test_support::only_error;
using statement_test_support::parse_module;

void expect_error(const std::string& source, const std::string& message, int line, int column) {
    const diagnostics::Diagnostic diagnostic =
        only_error(statement_test_support::parse_module(source));
    EXPECT_EQ(diagnostic.code, "SyntaxError") << source;
    EXPECT_EQ(diagnostic.message, message) << source;
    EXPECT_EQ(diagnostic.line, line) << source;
    EXPECT_EQ(diagnostic.column, column) << source;
    EXPECT_EQ(diagnostic.severity, diagnostics::Severity::Error) << source;
}

TEST(StatementParserError, JunkAfterASimpleStatementIsReportedOnce) {
    expect_error("pass pass\n", "expected a newline after the statement", 1, 6);
}

TEST(StatementParserError, ABadStatementDoesNotStopTheOnesAfterIt) {
    const statement_test_support::ModuleResult result = parse_module("pass pass\nbreak\n");

    EXPECT_EQ(result.diagnostics.size(), 1u);
    ASSERT_NE(result.module, nullptr);
    // The bad statement is dropped; the good one after it survives. This is
    // what panic-mode recovery is for, and asserting the tree rather than
    // only the diagnostic count is what actually pins it.
    EXPECT_EQ(result.printed(), "(Module\n  (Break))");
}

TEST(StatementParserError, ParseModuleIsNeverNullEvenWhenEverythingFailed) {
    const statement_test_support::ModuleResult result = parse_module("pass pass\n");

    ASSERT_NE(result.module, nullptr);
    EXPECT_EQ(result.printed(), "(Module)");
    EXPECT_TRUE(result.module->body().empty());
}

TEST(StatementParserError, ABadReturnValueReportsExactlyOneDiagnostic) {
    // ExpressionParser already reported "lambda expressions are not
    // supported". The statement parser must propagate the failure silently
    // rather than adding a second message.
    expect_error("return lambda: 1\n", "lambda expressions are not supported", 1, 8);
}

TEST(StatementParserError, AssigningToALiteralIsRejected) {
    expect_error("1 = x\n", "cannot assign to literal", 1, 1);
}

TEST(StatementParserError, AssigningToACallIsRejected) {
    expect_error("f() = x\n", "cannot assign to function call", 1, 1);
}

TEST(StatementParserError, AnnotatingATupleIsRejected) {
    // Python does not allow annotating a tuple, and AnnAssign holds one
    // target, so this is rejected in the parser rather than deferred.
    expect_error("x, y: int = 1\n", "only single targets can be annotated", 1, 1);
}

TEST(StatementParserError, ChainedAssignmentGetsTheGenericMessage) {
    // Deliberately not a named diagnostic: import and augmented assignment
    // appear in essentially every real file and earn specific messages, while
    // chained assignment is rare enough that the generic one is adequate.
    expect_error("a = b = 1\n", "expected a newline after the statement", 1, 7);
}

TEST(StatementParserError, AFailedExpressionStatementReportsOnlyItsOwnDiagnostic) {
    expect_error("f(*a)\n", "starred expressions are not supported", 1, 3);
}

TEST(StatementParserError, AnIndentWithNoBlockHeaderIsReported) {
    // IndentationPass emits a balanced INDENT/DEDENT pair here and reports
    // nothing, so this diagnostic is the parser's to produce.
    const diagnostics::Diagnostic diagnostic =
        only_error(statement_test_support::parse_module("a = 1\n    b = 2\nc = 3\n"));
    EXPECT_EQ(diagnostic.code, "IndentationError");
    EXPECT_EQ(diagnostic.message, "unexpected indent");
    EXPECT_EQ(diagnostic.line, 2);
    EXPECT_EQ(diagnostic.column, 5);
}

TEST(StatementParserError, AnUnexpectedIndentDoesNotEatTheRestOfTheFile) {
    const statement_test_support::ModuleResult result =
        parse_module("a = 1\n    b = 2\nc = 3\n");

    ASSERT_NE(result.module, nullptr);
    // The indented block is discarded; the statements around it survive.
    EXPECT_EQ(result.printed(),
              "(Module\n  (Assign (Name a) (Constant 1))\n  (Assign (Name c) (Constant 3)))");
}

TEST(StatementParserError, ANestedUnexpectedIndentIsStillOneDiagnostic) {
    // skip_unexpected_block tracks nesting depth, so a block containing its
    // own deeper block is skipped whole rather than re-reported per level.
    const statement_test_support::ModuleResult result =
        parse_module("a = 1\n    b = 2\n        c = 3\nd = 4\n");

    EXPECT_EQ(result.diagnostics.size(), 1u);
    ASSERT_NE(result.module, nullptr);
    EXPECT_EQ(result.printed(),
              "(Module\n  (Assign (Name a) (Constant 1))\n  (Assign (Name d) (Constant 4)))");
}

TEST(StatementParserError, AMissingColonAfterAnIfConditionIsReported) {
    expect_error("if x\n    pass\n", "expected ':'", 1, 5);
}

TEST(StatementParserError, AnIfHeaderWithNoIndentedBodyIsReported) {
    expect_error("if x:\npass\n", "expected an indented block", 2, 1);
}

TEST(StatementParserError, ABadStatementInsideASuiteDoesNotDiscardTheSuite) {
    // The reason recovery stops at -- and never consumes -- INDENT/DEDENT.
    const statement_test_support::ModuleResult result =
        parse_module("if a:\n    pass pass\n    break\n");

    EXPECT_EQ(result.diagnostics.size(), 1u);
    ASSERT_NE(result.module, nullptr);
    EXPECT_EQ(result.printed(), "(Module\n  (If (Name a)\n    (Break)))");
}

TEST(StatementParserError, ABadSuiteDoesNotSwallowTheStatementsAfterIt) {
    const statement_test_support::ModuleResult result =
        parse_module("if a:\n    pass pass\nb = 1\n");

    EXPECT_EQ(result.diagnostics.size(), 1u);
    ASSERT_NE(result.module, nullptr);
    EXPECT_EQ(result.printed(), "(Module\n  (Assign (Name b) (Constant 1)))");
}

TEST(StatementParserError, AFailedElseHeaderIsStillOneDiagnostic) {
    // parse_else_clause must distinguish "no else" from "broken else". If it
    // cannot, parse_if builds a valid If out of a failed parse, its recovery
    // path never runs, and the orphaned block is reported a second time.
    expect_error("if a:\n    pass\nelse\n    pass\n", "expected ':'", 3, 5);
}

TEST(StatementParserError, ACommentBeforeARecoveredStatementDoesNotHideTheBoundary) {
    // A comment-only line emits no NEWLINE and survives IndentationPass, so
    // the token physically before the cursor is not the one that logically
    // precedes it. Without the backward comment skip, at_statement_boundary()
    // answers false here, synchronize() eats the `pass` line, and the tree
    // silently loses it.
    const statement_test_support::ModuleResult result =
        parse_module("if x:\n# note\npass\n");

    EXPECT_EQ(result.diagnostics.size(), 1u);
    EXPECT_EQ(result.diagnostics.front().message, "expected an indented block");
    ASSERT_NE(result.module, nullptr);
    EXPECT_EQ(result.printed(), "(Module\n  (Pass))");
}

TEST(StatementParserError, AFailedIfConditionSwallowsItsOrphanedBlock) {
    expect_error("if lambda: 1\n    pass\n", "lambda expressions are not supported", 1, 4);
}

TEST(StatementParserError, AForWithNoInKeywordIsReported) {
    expect_error("for x:\n    pass\n", "expected 'in' after the for target", 1, 6);
}

TEST(StatementParserError, AForTargetThatCannotBeAssignedIsReported) {
    expect_error("for f() in items:\n    pass\n", "cannot assign to function call", 1, 5);
}

} // namespace
} // namespace cythonpp::domain::parser
