#include <gtest/gtest.h>

#include "domain/ast/return.h"
#include "statement_parse_test_helpers.h"

namespace cythonpp::domain::parser {
namespace {

using statement_test_support::parse_module;
using statement_test_support::printed;

TEST(StatementParser, AnEmptyFileIsAnEmptyModule) {
    EXPECT_EQ(printed(""), "(Module)");
}

TEST(StatementParser, ACommentOnlyFileIsAnEmptyModule) {
    // A comment-only line emits no NEWLINE and no indentation, so it is
    // invisible as a line boundary. The module must still come back empty
    // rather than the parser tripping over the COMMENT_SINGLE token.
    EXPECT_EQ(printed("# nothing here\n"), "(Module)");
}

TEST(StatementParser, ParsesPass) {
    EXPECT_EQ(printed("pass\n"), "(Module\n  (Pass))");
}

TEST(StatementParser, ParsesBreakAndContinue) {
    EXPECT_EQ(printed("break\n"), "(Module\n  (Break))");
    EXPECT_EQ(printed("continue\n"), "(Module\n  (Continue))");
}

TEST(StatementParser, ParsesSeveralStatementsInSourceOrder) {
    EXPECT_EQ(printed("pass\nbreak\ncontinue\n"),
              "(Module\n  (Pass)\n  (Break)\n  (Continue))");
}

TEST(StatementParser, AFileWithNoTrailingNewlineStillParses) {
    EXPECT_EQ(printed("pass"), "(Module\n  (Pass))");
}

TEST(StatementParser, SemicolonsSeparateStatementsOnOneLine) {
    EXPECT_EQ(printed("pass; break; continue\n"),
              "(Module\n  (Pass)\n  (Break)\n  (Continue))");
}

TEST(StatementParser, ATrailingSemicolonIsAllowed) {
    EXPECT_EQ(printed("pass;\n"), "(Module\n  (Pass))");
}

TEST(StatementParser, BlankLinesBetweenStatementsAreInvisible) {
    EXPECT_EQ(printed("pass\n\n\nbreak\n"), "(Module\n  (Pass)\n  (Break))");
}

TEST(StatementParser, ACommentBetweenStatementsIsInvisible) {
    EXPECT_EQ(printed("pass\n# why\nbreak\n"), "(Module\n  (Pass)\n  (Break))");
}

TEST(StatementParser, ParsesABareReturn) {
    EXPECT_EQ(printed("return\n"), "(Module\n  (Return))");
}

TEST(StatementParser, ParsesAReturnWithAValue) {
    EXPECT_EQ(printed("return 1\n"), "(Module\n  (Return (Constant 1)))");
}

TEST(StatementParser, ParsesAReturnOfATuple) {
    // parse_expression_list rather than parse_expression: `return a, b`
    // returns one tuple, not the first of two values.
    EXPECT_EQ(printed("return a, b\n"),
              "(Module\n  (Return (TupleExpr (Name a) (Name b))))");
}

TEST(StatementParser, ParsesAReturnOfAnOperatorExpression) {
    EXPECT_EQ(printed("return a + b * 2\n"),
              "(Module\n  (Return (BinOp + (Name a) (BinOp * (Name b) (Constant 2)))))");
}

TEST(StatementParser, AReturnCanShareALineViaASemicolon) {
    EXPECT_EQ(printed("pass; return 1\n"),
              "(Module\n  (Pass)\n  (Return (Constant 1)))");
}

TEST(StatementParser, ABareReturnHasNoValue) {
    const statement_test_support::ModuleResult result = parse_module("return\n");
    ASSERT_NE(result.module, nullptr);
    ASSERT_EQ(result.module->body().size(), 1u);
    const auto* returned = dynamic_cast<const ast::Return*>(result.module->body().front().get());
    ASSERT_NE(returned, nullptr);
    // value() dereferences unconditionally, so has_value() is the only
    // supported way to ask -- calling value() here would dereference null.
    EXPECT_FALSE(returned->has_value());
}

} // namespace
} // namespace cythonpp::domain::parser
