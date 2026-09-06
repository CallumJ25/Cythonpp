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

} // namespace
} // namespace cythonpp::domain::parser
