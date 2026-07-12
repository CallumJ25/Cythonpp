#include <gtest/gtest.h>

#include "domain/lexer/token_category.h"
#include "domain/lexer/token_type.h"

namespace cythonpp::domain::lexer {
namespace {

TEST(TokenCategory, SingleCategoryTokensMatchExactlyOneCategory) {
    EXPECT_TRUE(has_category(token_type::KEYWORD_DEF, token_category::KEYWORD));
    EXPECT_FALSE(has_category(token_type::KEYWORD_DEF, token_category::NUMBER));
    EXPECT_FALSE(has_category(token_type::KEYWORD_DEF, token_category::IDENTIFIER));

    EXPECT_TRUE(has_category(token_type::LITERAL_INT, token_category::NUMBER));
    EXPECT_FALSE(has_category(token_type::LITERAL_INT, token_category::KEYWORD));

    EXPECT_TRUE(has_category(token_type::OP_PLUS, token_category::OPERATOR));
    EXPECT_TRUE(has_category(token_type::OPEN_PAREN, token_category::DELIMITER));
    EXPECT_TRUE(has_category(token_type::COMMENT_SINGLE, token_category::PUNCTUATION));
    EXPECT_TRUE(has_category(token_type::TOKEN_EOF, token_category::SPECIAL));
}

TEST(TokenCategory, BoolTrueFalseAreKeywordAndNumber) {
    EXPECT_TRUE(has_category(token_type::BOOL_TRUE, token_category::KEYWORD));
    EXPECT_TRUE(has_category(token_type::BOOL_TRUE, token_category::NUMBER));
    EXPECT_FALSE(has_category(token_type::BOOL_TRUE, token_category::STRING));

    EXPECT_TRUE(has_category(token_type::BOOL_FALSE, token_category::KEYWORD));
    EXPECT_TRUE(has_category(token_type::BOOL_FALSE, token_category::NUMBER));
}

TEST(TokenCategory, TypeAnnotationKeywordsAreKeywordAndIdentifier) {
    EXPECT_TRUE(has_category(token_type::TYPE_INT, token_category::KEYWORD));
    EXPECT_TRUE(has_category(token_type::TYPE_INT, token_category::IDENTIFIER));
    EXPECT_FALSE(has_category(token_type::TYPE_INT, token_category::NUMBER));

    EXPECT_TRUE(has_category(token_type::TYPE_TUPLE, token_category::KEYWORD));
    EXPECT_TRUE(has_category(token_type::TYPE_TUPLE, token_category::IDENTIFIER));
}

TEST(TokenCategory, CategoryOfRecoversAllSetFlags) {
    uint16_t bool_true_flags = category_of(token_type::BOOL_TRUE);
    uint16_t expected = static_cast<uint16_t>(token_category::KEYWORD) |
                        static_cast<uint16_t>(token_category::NUMBER);
    EXPECT_EQ(bool_true_flags, expected);
}

TEST(TokenCategory, DistinctSubtypesInSameCategoryAreNotEqual) {
    EXPECT_NE(static_cast<uint16_t>(token_type::LITERAL_INT), static_cast<uint16_t>(token_type::LITERAL_FLOAT));
    EXPECT_NE(static_cast<uint16_t>(token_type::KEYWORD_DEF), static_cast<uint16_t>(token_type::KEYWORD_CLASS));
}

} // namespace
} // namespace cythonpp::domain::lexer
