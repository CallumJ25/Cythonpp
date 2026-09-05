#include <gtest/gtest.h>

#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>

#include "domain/lexer/lexer.h"
#include "domain/lexer/token_stream.h"

namespace cythonpp::domain::lexer {
namespace {

std::vector<Token> sample_tokens() {
    return {
        Token(token_type::IDENTIFIER, "x", 1, 1, 1, 2),
        Token(token_type::OP_ASSIGN, "=", 1, 3, 1, 4),
        Token(token_type::LITERAL_INT, "5", 1, 5, 1, 6),
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

TokenStream stream_of(const std::string& source) {
    return TokenStream(Lexer(source).tokenize());
}

TEST(TokenStreamCursor, PeekReadsWithoutAdvancing) {
    TokenStream stream = stream_of("a + b");
    EXPECT_EQ(stream.peek().type(), token_type::IDENTIFIER);
    EXPECT_EQ(stream.peek().lexeme(), "a");
    EXPECT_EQ(stream.peek().type(), token_type::IDENTIFIER);
    EXPECT_EQ(stream.position(), 0u);
}

TEST(TokenStreamCursor, PeekLooksAhead) {
    TokenStream stream = stream_of("a + b");
    EXPECT_EQ(stream.peek(1).type(), token_type::OP_PLUS);
    EXPECT_EQ(stream.peek(2).lexeme(), "b");
}

TEST(TokenStreamCursor, PeekClampsToTheLastToken) {
    TokenStream stream = stream_of("a");
    EXPECT_EQ(stream.peek(99).type(), token_type::TOKEN_EOF);
}

TEST(TokenStreamCursor, PeekOnAnEmptyStreamThrows) {
    TokenStream stream;
    EXPECT_THROW((void)stream.peek(), std::out_of_range);
}

TEST(TokenStreamCursor, AdvanceReturnsTheCurrentTokenAndMovesOn) {
    TokenStream stream = stream_of("a + b");
    EXPECT_EQ(stream.advance().lexeme(), "a");
    EXPECT_EQ(stream.peek().type(), token_type::OP_PLUS);
}

TEST(TokenStreamCursor, AdvanceAtEndOfFileIsANoOp) {
    TokenStream stream = stream_of("a");
    while (!stream.at_end()) {
        stream.advance();
    }
    const std::size_t settled = stream.position();
    EXPECT_EQ(stream.advance().type(), token_type::TOKEN_EOF);
    EXPECT_EQ(stream.position(), settled) << "advance() ran past TOKEN_EOF";
}

TEST(TokenStreamCursor, CheckDoesNotConsume) {
    TokenStream stream = stream_of("a");
    EXPECT_TRUE(stream.check(token_type::IDENTIFIER));
    EXPECT_FALSE(stream.check(token_type::OP_PLUS));
    EXPECT_EQ(stream.position(), 0u);
}

TEST(TokenStreamCursor, MatchConsumesOnlyOnSuccess) {
    TokenStream stream = stream_of("a + b");
    EXPECT_FALSE(stream.match(token_type::OP_PLUS));
    EXPECT_EQ(stream.position(), 0u);
    EXPECT_TRUE(stream.match(token_type::IDENTIFIER));
    EXPECT_EQ(stream.peek().type(), token_type::OP_PLUS);
}

TEST(TokenStreamCursor, PositionAndSeekRoundTrip) {
    TokenStream stream = stream_of("a + b");
    stream.advance();
    const std::size_t saved = stream.position();
    stream.advance();
    stream.advance();
    stream.seek(saved);
    EXPECT_EQ(stream.peek().type(), token_type::OP_PLUS);
}

TEST(TokenStreamCursor, RewindReturnsToTheStart) {
    TokenStream stream = stream_of("a + b");
    stream.advance();
    stream.advance();
    stream.rewind();
    EXPECT_EQ(stream.peek().lexeme(), "a");
}

TEST(TokenStreamCursor, SeekPastTheEndClampsRatherThanThrowing) {
    TokenStream stream = stream_of("a");
    stream.seek(999);
    EXPECT_TRUE(stream.at_end());
}

TEST(TokenStreamCursor, CursorNeverRestsOnAComment) {
    // Inside brackets the lexer suppresses the newline, so the comment lands
    // in the middle of an expression -- which is exactly where a parser would
    // trip over it.
    TokenStream stream = stream_of("f(a,  # note\n    b)");
    std::vector<token_type> seen;
    while (!stream.at_end()) {
        seen.push_back(stream.advance().type());
    }
    EXPECT_EQ(std::count(seen.begin(), seen.end(), token_type::COMMENT_SINGLE), 0)
        << "the cursor stopped on a comment token";
}

TEST(TokenStreamCursor, AStreamBeginningWithACommentStartsPastIt) {
    TokenStream stream = stream_of("# leading\nx");
    EXPECT_EQ(stream.peek().lexeme(), "x");
}

TEST(TokenStreamCursor, TheExistingAccessorsStillSeeEveryToken) {
    // size(), at(), and iteration are the raw stream. Only the cursor skips
    // trivia; a later pass that wants to reprint source needs the comments.
    TokenStream stream = stream_of("# leading\nx");
    const auto comments = std::count_if(stream.begin(), stream.end(), [](const Token& token) {
        return token.type() == token_type::COMMENT_SINGLE;
    });
    EXPECT_EQ(comments, 1);
}

} // namespace
} // namespace cythonpp::domain::lexer
