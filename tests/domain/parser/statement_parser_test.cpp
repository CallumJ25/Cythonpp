#include <gtest/gtest.h>

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

} // namespace
} // namespace cythonpp::domain::parser
