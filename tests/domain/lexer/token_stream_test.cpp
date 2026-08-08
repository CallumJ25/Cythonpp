#include <gtest/gtest.h>

#include <stdexcept>
#include <vector>

#include "domain/lexer/token_stream.h"

namespace cythonpp::domain::lexer {
namespace {

std::vector<Token> sample_tokens() {
    return {
        Token(token_type::IDENTIFIER, "x", 1, 1),
        Token(token_type::OP_ASSIGN, "=", 1, 3),
        Token(token_type::LITERAL_INT, "5", 1, 5),
    };
}

TEST(TokenStream, DefaultConstructedStreamIsEmpty) {
    TokenStream stream;
    EXPECT_TRUE(stream.empty());
    EXPECT_EQ(stream.size(), 0u);
    EXPECT_EQ(stream.begin(), stream.end());
}

TEST(TokenStream, SizeAndEmptyReflectTheOwnedTokens) {
    TokenStream stream(sample_tokens());
    EXPECT_FALSE(stream.empty());
    EXPECT_EQ(stream.size(), 3u);
}

TEST(TokenStream, AtReturnsTheTokenAtTheGivenIndex) {
    TokenStream stream(sample_tokens());
    EXPECT_EQ(stream.at(0).type(), token_type::IDENTIFIER);
    EXPECT_EQ(stream.at(0).lexeme(), "x");
    EXPECT_EQ(stream.at(2).type(), token_type::LITERAL_INT);
    EXPECT_EQ(stream.at(2).column_number(), 5);
}

TEST(TokenStream, AtThrowsWhenTheIndexIsPastTheEnd) {
    TokenStream stream(sample_tokens());
    EXPECT_THROW(stream.at(3), std::out_of_range);
}

TEST(TokenStream, RangeForIterationVisitsTokensInOrder) {
    TokenStream stream(sample_tokens());

    std::vector<token_type> seen;
    for (const Token& token : stream) {
        seen.push_back(token.type());
    }

    ASSERT_EQ(seen.size(), 3u);
    EXPECT_EQ(seen[0], token_type::IDENTIFIER);
    EXPECT_EQ(seen[1], token_type::OP_ASSIGN);
    EXPECT_EQ(seen[2], token_type::LITERAL_INT);
}

TEST(TokenStream, TokensAccessorExposesTheUnderlyingVector) {
    TokenStream stream(sample_tokens());
    EXPECT_EQ(stream.tokens().size(), 3u);
    EXPECT_EQ(stream.tokens().front().lexeme(), "x");
}

} // namespace
} // namespace cythonpp::domain::lexer
