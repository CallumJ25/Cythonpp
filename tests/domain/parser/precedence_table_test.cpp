#include <cstdint>
#include <optional>

#include <gtest/gtest.h>

#include "domain/lexer/token_type.h"
#include "domain/parser/precedence_table.h"

namespace cythonpp::domain::parser {
namespace {

using lexer::token_type;

int level_of(token_type type) {
    const std::optional<BinaryPrecedence> precedence = binary_precedence_of(type);
    EXPECT_TRUE(precedence.has_value()) << "expected a binary operator";
    return precedence ? precedence->level : -1;
}

TEST(PrecedenceTable, EachOperatorSitsAtItsPythonLevel) {
    EXPECT_EQ(level_of(token_type::OP_PIPE), 1);
    EXPECT_EQ(level_of(token_type::OP_CARET), 2);
    EXPECT_EQ(level_of(token_type::OP_AMPERSAND), 3);
    EXPECT_EQ(level_of(token_type::OP_LEFT_SHIFT), 4);
    EXPECT_EQ(level_of(token_type::OP_RIGHT_SHIFT), 4);
    EXPECT_EQ(level_of(token_type::OP_PLUS), 5);
    EXPECT_EQ(level_of(token_type::OP_MINUS), 5);
    EXPECT_EQ(level_of(token_type::OP_STAR), 6);
    EXPECT_EQ(level_of(token_type::OP_AT), 6);
    EXPECT_EQ(level_of(token_type::OP_SLASH), 6);
    EXPECT_EQ(level_of(token_type::OP_DOUBLE_SLASH), 6);
    EXPECT_EQ(level_of(token_type::OP_PERCENT), 6);
}

TEST(PrecedenceTable, LevelsIncreaseAcrossEveryBoundary) {
    EXPECT_LT(level_of(token_type::OP_PIPE), level_of(token_type::OP_CARET));
    EXPECT_LT(level_of(token_type::OP_CARET), level_of(token_type::OP_AMPERSAND));
    EXPECT_LT(level_of(token_type::OP_AMPERSAND), level_of(token_type::OP_LEFT_SHIFT));
    EXPECT_LT(level_of(token_type::OP_LEFT_SHIFT), level_of(token_type::OP_PLUS));
    EXPECT_LT(level_of(token_type::OP_PLUS), level_of(token_type::OP_STAR));
}

TEST(PrecedenceTable, LowestLevelConstantMatchesTheLoosestOperator) {
    EXPECT_EQ(LOWEST_BINARY_LEVEL, level_of(token_type::OP_PIPE));
}

TEST(PrecedenceTable, OperatorFlaggedTokensThatAreNotBinaryReturnNullopt) {
    // The whole point of the table. has_category(OPERATOR) is true for every
    // one of these, so a parser that trusted the flag alone would treat `=`
    // as a binary operator and silently build a BinOp out of an assignment.
    const token_type not_binary[] = {
        token_type::OP_ASSIGN,
        token_type::OP_PLUS_ASSIGN,
        token_type::OP_MINUS_ASSIGN,
        token_type::OP_STAR_ASSIGN,
        token_type::OP_SLASH_ASSIGN,
        token_type::OP_DOUBLE_SLASH_ASSIGN,
        token_type::OP_PERCENT_ASSIGN,
        token_type::OP_DOUBLE_STAR_ASSIGN,
        token_type::OP_AT_ASSIGN,
        token_type::OP_AMPERSAND_ASSIGN,
        token_type::OP_PIPE_ASSIGN,
        token_type::OP_CARET_ASSIGN,
        token_type::OP_LEFT_SHIFT_ASSIGN,
        token_type::OP_RIGHT_SHIFT_ASSIGN,
        token_type::OP_WALRUS,
        token_type::OP_ARROW,
        token_type::OP_TILDE,
        token_type::OP_NOT,
        token_type::OP_AND,
        token_type::OP_OR,
        token_type::OP_IS,
        token_type::OP_IN,
        token_type::OP_NOT_IN,
        token_type::OP_IS_NOT,
        token_type::OP_EQUAL,
        token_type::OP_NOT_EQUAL,
        token_type::OP_LESS,
        token_type::OP_GREATER,
        token_type::OP_LESS_EQUAL,
        token_type::OP_GREATER_EQUAL,
        // '**' is right-associative and its interaction with unary operators
        // is asymmetric, so ExpressionParser owns it rather than the table.
        token_type::OP_DOUBLE_STAR,
    };
    for (const token_type type : not_binary) {
        EXPECT_FALSE(binary_precedence_of(type).has_value())
            << "unexpectedly in the table: " << static_cast<uint32_t>(type);
    }
}

TEST(PrecedenceTable, NonOperatorsReturnNullopt) {
    EXPECT_FALSE(binary_precedence_of(token_type::IDENTIFIER).has_value());
    EXPECT_FALSE(binary_precedence_of(token_type::COMMA).has_value());
    EXPECT_FALSE(binary_precedence_of(token_type::OPEN_PAREN).has_value());
    EXPECT_FALSE(binary_precedence_of(token_type::NEWLINE).has_value());
    EXPECT_FALSE(binary_precedence_of(token_type::KEYWORD_IF).has_value());
}

} // namespace
} // namespace cythonpp::domain::parser
