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

} // namespace
} // namespace cythonpp::domain::parser
