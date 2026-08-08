#include <gtest/gtest.h>

#include <string_view>

#include "domain/lexer/operator_table.h"

namespace cythonpp::domain::lexer {
namespace {

TEST(OperatorTable, MaximalMunchPrefersTheLongestMatch) {
    EXPECT_EQ(longest_operator_at("**=").type, token_type::OP_DOUBLE_STAR_ASSIGN);
    EXPECT_EQ(longest_operator_at("**=").length, 3u);
    EXPECT_EQ(longest_operator_at("**").type, token_type::OP_DOUBLE_STAR);
    EXPECT_EQ(longest_operator_at("*=").type, token_type::OP_STAR_ASSIGN);
    EXPECT_EQ(longest_operator_at("*").type, token_type::OP_STAR);

    EXPECT_EQ(longest_operator_at("//=").type, token_type::OP_DOUBLE_SLASH_ASSIGN);
    EXPECT_EQ(longest_operator_at("//").type, token_type::OP_DOUBLE_SLASH);
    EXPECT_EQ(longest_operator_at("/=").type, token_type::OP_SLASH_ASSIGN);
    EXPECT_EQ(longest_operator_at("/").type, token_type::OP_SLASH);

    EXPECT_EQ(longest_operator_at(">>=").type, token_type::OP_RIGHT_SHIFT_ASSIGN);
    EXPECT_EQ(longest_operator_at(">>").type, token_type::OP_RIGHT_SHIFT);
    EXPECT_EQ(longest_operator_at(">=").type, token_type::OP_GREATER_EQUAL);
    EXPECT_EQ(longest_operator_at(">").type, token_type::OP_GREATER);

    EXPECT_EQ(longest_operator_at("<<=").type, token_type::OP_LEFT_SHIFT_ASSIGN);
    EXPECT_EQ(longest_operator_at("<<").type, token_type::OP_LEFT_SHIFT);
    EXPECT_EQ(longest_operator_at("<=").type, token_type::OP_LESS_EQUAL);
    EXPECT_EQ(longest_operator_at("<").type, token_type::OP_LESS);
}

TEST(OperatorTable, TrailingContextDoesNotExtendTheMatch) {
    EXPECT_EQ(longest_operator_at("+=1").type, token_type::OP_PLUS_ASSIGN);
    EXPECT_EQ(longest_operator_at("+=1").length, 2u);

    EXPECT_EQ(longest_operator_at("**kwargs").type, token_type::OP_DOUBLE_STAR);
    EXPECT_EQ(longest_operator_at("**kwargs").length, 2u);

    EXPECT_EQ(longest_operator_at("->int").type, token_type::OP_ARROW);
    EXPECT_EQ(longest_operator_at("->int").length, 2u);

    EXPECT_EQ(longest_operator_at("===").type, token_type::OP_EQUAL);
    EXPECT_EQ(longest_operator_at("===").length, 2u);
}

// The buffer-overrun guard: a one-character view into a longer buffer must
// not read the character after it.
TEST(OperatorTable, MatchesNeverReadPastTheEndOfTheText) {
    EXPECT_EQ(longest_operator_at(std::string_view("*", 1)).type, token_type::OP_STAR);
    EXPECT_EQ(longest_operator_at(std::string_view("*", 1)).length, 1u);

    EXPECT_EQ(longest_operator_at(std::string_view("**=", 1)).type, token_type::OP_STAR);
    EXPECT_EQ(longest_operator_at(std::string_view("**=", 2)).type, token_type::OP_DOUBLE_STAR);
    EXPECT_EQ(longest_operator_at(std::string_view(">>=", 2)).type, token_type::OP_RIGHT_SHIFT);
}

TEST(OperatorTable, EllipsisWinsOverRepeatedDots) {
    EXPECT_EQ(longest_operator_at("...").type, token_type::ELLIPSIS);
    EXPECT_EQ(longest_operator_at("...").length, 3u);

    // ".." is deliberately not a token: maximal munch finds "." twice.
    EXPECT_EQ(longest_operator_at("..").type, token_type::DOT);
    EXPECT_EQ(longest_operator_at("..").length, 1u);
    EXPECT_EQ(longest_operator_at(".").type, token_type::DOT);
    EXPECT_EQ(longest_operator_at("....").type, token_type::ELLIPSIS);
    EXPECT_EQ(longest_operator_at("....").length, 3u);
}

TEST(OperatorTable, WalrusAndArrowAreDistinctFromTheirPrefixes) {
    EXPECT_EQ(longest_operator_at(":=").type, token_type::OP_WALRUS);
    EXPECT_EQ(longest_operator_at(":").type, token_type::COLON);
    EXPECT_EQ(longest_operator_at("-=").type, token_type::OP_MINUS_ASSIGN);
    EXPECT_EQ(longest_operator_at("->").type, token_type::OP_ARROW);
    EXPECT_EQ(longest_operator_at("-").type, token_type::OP_MINUS);
}

TEST(OperatorTable, DelimitersAndPunctuationShareTheLookup) {
    EXPECT_EQ(longest_operator_at("(").type, token_type::OPEN_PAREN);
    EXPECT_EQ(longest_operator_at(")").type, token_type::CLOSE_PAREN);
    EXPECT_EQ(longest_operator_at("[").type, token_type::OPEN_BRACKET);
    EXPECT_EQ(longest_operator_at("]").type, token_type::CLOSE_BRACKET);
    EXPECT_EQ(longest_operator_at("{").type, token_type::OPEN_BRACE);
    EXPECT_EQ(longest_operator_at("}").type, token_type::CLOSE_BRACE);
    EXPECT_EQ(longest_operator_at(",").type, token_type::COMMA);
    EXPECT_EQ(longest_operator_at(";").type, token_type::SEMICOLON);

    EXPECT_EQ(longest_operator_at("(").length, 1u);
    EXPECT_EQ(longest_operator_at("}").length, 1u);
}

// Documents the intentional gap: only "!=" exists in Python, never a lone
// '!', so the scanner must be the one to report it.
TEST(OperatorTable, LoneBangDoesNotMatch) {
    EXPECT_EQ(longest_operator_at("!=").type, token_type::OP_NOT_EQUAL);
    EXPECT_EQ(longest_operator_at("!=").length, 2u);
    EXPECT_EQ(longest_operator_at("!").length, 0u);
    EXPECT_EQ(longest_operator_at("!x").length, 0u);
}

TEST(OperatorTable, UnknownTextProducesNoMatch) {
    EXPECT_EQ(longest_operator_at("").length, 0u);
    EXPECT_EQ(longest_operator_at("$").length, 0u);
    EXPECT_EQ(longest_operator_at("abc").length, 0u);
    EXPECT_EQ(longest_operator_at("#comment").length, 0u);
    EXPECT_EQ(longest_operator_at("?").length, 0u);
    EXPECT_EQ(longest_operator_at("\\").length, 0u);
    EXPECT_EQ(longest_operator_at("").type, token_type::TOKEN_ERROR);
}

TEST(OperatorTable, IsOperatorStartAgreesWithTheTable) {
    for (char c : std::string_view("!%&()*+,-./:;<=>@[]^{|}~")) {
        EXPECT_TRUE(is_operator_start(c)) << "expected '" << c << "' to start an operator";
    }
    for (char c : std::string_view("aZ0_ \t\n#?\\\"'$")) {
        EXPECT_FALSE(is_operator_start(c)) << "did not expect '" << c << "' to start an operator";
    }
}

TEST(OperatorTable, LexemeLookupRoundTripsThroughTheTable) {
    EXPECT_EQ(operator_lexeme_of(token_type::OP_DOUBLE_STAR_ASSIGN), "**=");
    EXPECT_EQ(operator_lexeme_of(token_type::ELLIPSIS), "...");
    EXPECT_EQ(operator_lexeme_of(token_type::OPEN_BRACE), "{");
    // Unmapped by design: '?' has no Python spelling.
    EXPECT_TRUE(operator_lexeme_of(token_type::QUESTION).empty());
    EXPECT_TRUE(operator_lexeme_of(token_type::KEYWORD_IF).empty());
}

} // namespace
} // namespace cythonpp::domain::lexer
