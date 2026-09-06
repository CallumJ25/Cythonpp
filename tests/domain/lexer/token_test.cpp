#include <gtest/gtest.h>

#include <cstdint>
#include <iterator>

#include "domain/lexer/lexer.h"
#include "domain/lexer/token_category.h"
#include "domain/lexer/token_type.h"
#include "domain/lexer/token_type_name.h"

namespace cythonpp::domain::lexer {
namespace {

// Hand-maintained because C++17 has no enum reflection. This array is the
// only thing standing between the packing scheme and two enumerators
// silently sharing a value, so a new enumerator must be added here too.
constexpr token_type ALL_TOKEN_TYPES[] = {
    token_type::TOKEN_EOF,
    token_type::TOKEN_ERROR,
    token_type::NEWLINE,
    token_type::SPACE,
    token_type::TAB,
    token_type::INDENT,
    token_type::DEDENT,

    token_type::IDENTIFIER,

    token_type::LITERAL_STRING,
    token_type::LITERAL_BYTES,
    token_type::FSTRING_START,
    token_type::FSTRING_MIDDLE,
    token_type::FSTRING_END,

    token_type::LITERAL_INT,
    token_type::LITERAL_FLOAT,
    token_type::LITERAL_COMPLEX,

    token_type::BOOL_TRUE,
    token_type::BOOL_FALSE,
    token_type::KEYWORD_NONE,
    token_type::ELLIPSIS,

    token_type::KEYWORD_IF,
    token_type::KEYWORD_ELSE,
    token_type::KEYWORD_FOR,
    token_type::KEYWORD_ELIF,
    token_type::KEYWORD_WHILE,
    token_type::KEYWORD_RETURN,
    token_type::KEYWORD_DEF,
    token_type::KEYWORD_CLASS,
    token_type::KEYWORD_AS,
    token_type::KEYWORD_ASSERT,
    token_type::KEYWORD_ASYNC,
    token_type::KEYWORD_AWAIT,
    token_type::KEYWORD_BREAK,
    token_type::KEYWORD_CONTINUE,
    token_type::KEYWORD_DEL,
    token_type::KEYWORD_EXCEPT,
    token_type::KEYWORD_FINALLY,
    token_type::KEYWORD_FROM,
    token_type::KEYWORD_GLOBAL,
    token_type::KEYWORD_IMPORT,
    token_type::KEYWORD_LAMBDA,
    token_type::KEYWORD_NONLOCAL,
    token_type::KEYWORD_PASS,
    token_type::KEYWORD_RAISE,
    token_type::KEYWORD_TRY,
    token_type::KEYWORD_WITH,
    token_type::KEYWORD_YIELD,
    token_type::KEYWORD_MATCH,
    token_type::KEYWORD_CASE,
    token_type::KEYWORD_UNDERSCORE,

    token_type::OP_AND,
    token_type::OP_OR,
    token_type::OP_NOT,
    token_type::OP_IS,
    token_type::OP_IN,
    token_type::OP_NOT_IN,
    token_type::OP_IS_NOT,

    token_type::OP_ASSIGN,
    token_type::OP_PLUS,
    token_type::OP_MINUS,
    token_type::OP_EQUAL,
    token_type::OP_NOT_EQUAL,
    token_type::OP_LESS,
    token_type::OP_GREATER,
    token_type::OP_LESS_EQUAL,
    token_type::OP_GREATER_EQUAL,
    token_type::OP_STAR,
    token_type::OP_SLASH,
    token_type::OP_DOUBLE_SLASH,
    token_type::OP_PERCENT,
    token_type::OP_DOUBLE_STAR,
    token_type::OP_AT,
    token_type::OP_AMPERSAND,
    token_type::OP_PIPE,
    token_type::OP_CARET,
    token_type::OP_TILDE,
    token_type::OP_LEFT_SHIFT,
    token_type::OP_RIGHT_SHIFT,
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
    token_type::OP_RIGHT_SHIFT_ASSIGN,
    token_type::OP_LEFT_SHIFT_ASSIGN,
    token_type::OP_WALRUS,
    token_type::OP_ARROW,

    token_type::OPEN_PAREN,
    token_type::CLOSE_PAREN,
    token_type::OPEN_BRACE,
    token_type::CLOSE_BRACE,
    token_type::OPEN_BRACKET,
    token_type::CLOSE_BRACKET,

    token_type::SEMICOLON,
    token_type::COMMA,
    token_type::DOT,
    token_type::COLON,
    token_type::QUESTION,
    token_type::COMMENT_SINGLE,
    token_type::COMMENT_MULTI,

    token_type::TYPE_INT,
    token_type::TYPE_FLOAT,
    token_type::TYPE_STRING,
    token_type::TYPE_BOOL,
    token_type::TYPE_LIST,
    token_type::TYPE_DICT,
    token_type::TYPE_SET,
    token_type::TYPE_TUPLE,
    token_type::TYPE_BYTES,
    token_type::TYPE_COMPLEX,
    token_type::TYPE_FROZENSET,
    token_type::TYPE_BYTEARRAY,
    token_type::TYPE_OBJECT,
};

uint32_t flags(token_category category) { return static_cast<uint32_t>(category); }

TEST(TokenCategory, CoreTokensCarryTheirPrimaryCategory) {
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
    uint32_t bool_true_flags = category_of(token_type::BOOL_TRUE);
    uint32_t expected = flags(token_category::KEYWORD) | flags(token_category::NUMBER) |
                        flags(token_category::OBJECT);
    EXPECT_EQ(bool_true_flags, expected);
}

TEST(TokenCategory, DistinctSubtypesInSameCategoryAreNotEqual) {
    EXPECT_NE(static_cast<uint32_t>(token_type::LITERAL_INT), static_cast<uint32_t>(token_type::LITERAL_FLOAT));
    EXPECT_NE(static_cast<uint32_t>(token_type::KEYWORD_DEF), static_cast<uint32_t>(token_type::KEYWORD_CLASS));
}

// The regression guard for the whole 16/16 widening: under the old 8-bit
// split, OBJECT (bit 8) shifted clean out of a uint16_t and every assertion
// below would read back as zero.
TEST(TokenCategory, ObjectBitSurvivesTheWidenedPacking) {
    EXPECT_NE(category_of(token_type::IDENTIFIER) & flags(token_category::OBJECT), 0u);
    EXPECT_NE(category_of(token_type::LITERAL_INT) & flags(token_category::OBJECT), 0u);
    EXPECT_NE(category_of(token_type::TYPE_INT) & flags(token_category::OBJECT), 0u);
    EXPECT_NE(category_of(token_type::ELLIPSIS), 0u);
    EXPECT_NE(static_cast<uint32_t>(token_type::ELLIPSIS), 0u);
}

TEST(TokenCategory, ObjectMarksEveryTokenDenotingAPythonValue) {
    EXPECT_TRUE(has_category(token_type::LITERAL_INT, token_category::OBJECT));
    EXPECT_TRUE(has_category(token_type::LITERAL_FLOAT, token_category::OBJECT));
    EXPECT_TRUE(has_category(token_type::LITERAL_COMPLEX, token_category::OBJECT));
    EXPECT_TRUE(has_category(token_type::LITERAL_STRING, token_category::OBJECT));
    EXPECT_TRUE(has_category(token_type::LITERAL_BYTES, token_category::OBJECT));
    EXPECT_TRUE(has_category(token_type::FSTRING_MIDDLE, token_category::OBJECT));
    EXPECT_TRUE(has_category(token_type::BOOL_TRUE, token_category::OBJECT));
    EXPECT_TRUE(has_category(token_type::BOOL_FALSE, token_category::OBJECT));
    EXPECT_TRUE(has_category(token_type::KEYWORD_NONE, token_category::OBJECT));
    EXPECT_TRUE(has_category(token_type::ELLIPSIS, token_category::OBJECT));
    EXPECT_TRUE(has_category(token_type::IDENTIFIER, token_category::OBJECT));
    EXPECT_TRUE(has_category(token_type::TYPE_INT, token_category::OBJECT));
    EXPECT_TRUE(has_category(token_type::TYPE_BYTES, token_category::OBJECT));
    EXPECT_TRUE(has_category(token_type::TYPE_OBJECT, token_category::OBJECT));
}

TEST(TokenCategory, ObjectIsAbsentFromSyntaxOnlyTokens) {
    EXPECT_FALSE(has_category(token_type::KEYWORD_IF, token_category::OBJECT));
    EXPECT_FALSE(has_category(token_type::KEYWORD_DEF, token_category::OBJECT));
    EXPECT_FALSE(has_category(token_type::KEYWORD_IMPORT, token_category::OBJECT));
    // `lambda: 1` produces an object, but the keyword alone denotes none.
    EXPECT_FALSE(has_category(token_type::KEYWORD_LAMBDA, token_category::OBJECT));
    EXPECT_FALSE(has_category(token_type::KEYWORD_MATCH, token_category::OBJECT));
    EXPECT_FALSE(has_category(token_type::OP_AND, token_category::OBJECT));
    EXPECT_FALSE(has_category(token_type::OP_IN, token_category::OBJECT));
    EXPECT_FALSE(has_category(token_type::OP_PLUS, token_category::OBJECT));
    EXPECT_FALSE(has_category(token_type::OP_WALRUS, token_category::OBJECT));
    EXPECT_FALSE(has_category(token_type::OPEN_PAREN, token_category::OBJECT));
    // Same rule as lambda: `[1, 2]` builds a list, but '[' alone does not.
    EXPECT_FALSE(has_category(token_type::OPEN_BRACKET, token_category::OBJECT));
    EXPECT_FALSE(has_category(token_type::COMMA, token_category::OBJECT));
    EXPECT_FALSE(has_category(token_type::COLON, token_category::OBJECT));
    EXPECT_FALSE(has_category(token_type::COMMENT_SINGLE, token_category::OBJECT));
    EXPECT_FALSE(has_category(token_type::TOKEN_EOF, token_category::OBJECT));
    EXPECT_FALSE(has_category(token_type::NEWLINE, token_category::OBJECT));
    EXPECT_FALSE(has_category(token_type::SPACE, token_category::OBJECT));
}

TEST(TokenCategory, ReservedWordOperatorsAreBothKeywordAndOperator) {
    EXPECT_TRUE(has_category(token_type::OP_AND, token_category::KEYWORD));
    EXPECT_TRUE(has_category(token_type::OP_AND, token_category::OPERATOR));
    EXPECT_FALSE(has_category(token_type::OP_AND, token_category::IDENTIFIER));

    EXPECT_TRUE(has_category(token_type::OP_IS, token_category::KEYWORD));
    EXPECT_TRUE(has_category(token_type::OP_IS, token_category::OPERATOR));
    EXPECT_TRUE(has_category(token_type::OP_IN, token_category::KEYWORD));
    EXPECT_TRUE(has_category(token_type::OP_IN, token_category::OPERATOR));

    EXPECT_TRUE(has_category(token_type::OP_PLUS, token_category::OPERATOR));
    EXPECT_FALSE(has_category(token_type::OP_PLUS, token_category::KEYWORD));
}

TEST(TokenCategory, EllipsisIsAValueNotPunctuation) {
    EXPECT_TRUE(has_category(token_type::ELLIPSIS, token_category::OBJECT));
    EXPECT_FALSE(has_category(token_type::ELLIPSIS, token_category::PUNCTUATION));
    EXPECT_FALSE(has_category(token_type::ELLIPSIS, token_category::DELIMITER));
}

TEST(TokenType, PackingKeepsFlagsAndSubtypeInSeparateHalves) {
    EXPECT_EQ(subtype_of(token_type::TOKEN_EOF), 0u);
    EXPECT_EQ(subtype_of(token_type::LITERAL_FLOAT), 1u);
    // The highest index in use: proves nothing overflows into the flag half.
    EXPECT_EQ(subtype_of(token_type::OP_ARROW), 35u);
    EXPECT_EQ(category_of(token_type::OP_ARROW), flags(token_category::OPERATOR));

    // One literal spot-check pins the layout: NUMBER|OBJECT in the high half.
    EXPECT_EQ(static_cast<uint32_t>(token_type::LITERAL_INT), 0x01010000u);
}

TEST(TokenType, SubtypeIndicesRestartPerFlagCombination) {
    EXPECT_EQ(subtype_of(token_type::LITERAL_INT), 0u);
    EXPECT_EQ(subtype_of(token_type::IDENTIFIER), 0u);
    EXPECT_EQ(subtype_of(token_type::TOKEN_EOF), 0u);
    EXPECT_EQ(subtype_of(token_type::BOOL_TRUE), 0u);

    EXPECT_NE(token_type::LITERAL_INT, token_type::IDENTIFIER);
    EXPECT_NE(token_type::LITERAL_INT, token_type::TOKEN_EOF);
    EXPECT_NE(token_type::LITERAL_INT, token_type::BOOL_TRUE);
    EXPECT_NE(token_type::IDENTIFIER, token_type::TOKEN_EOF);
    EXPECT_NE(token_type::IDENTIFIER, token_type::BOOL_TRUE);
    EXPECT_NE(token_type::TOKEN_EOF, token_type::BOOL_TRUE);
}

TEST(TokenType, EveryEnumeratorHasAUniqueValue) {
    const std::size_t count = std::size(ALL_TOKEN_TYPES);
    EXPECT_EQ(count, 119u);

    for (std::size_t left = 0; left < count; ++left) {
        for (std::size_t right = left + 1; right < count; ++right) {
            EXPECT_NE(static_cast<uint32_t>(ALL_TOKEN_TYPES[left]),
                      static_cast<uint32_t>(ALL_TOKEN_TYPES[right]))
                << "index " << left << " (" << token_type_name(ALL_TOKEN_TYPES[left])
                << ") aliases index " << right << " (" << token_type_name(ALL_TOKEN_TYPES[right]) << ")";
        }
    }
}

TEST(TokenType, BuiltinTypeNamesAreDistinctFromPlainIdentifiers) {
    // Both carry IDENTIFIER, so a "plain identifier?" check must compare the
    // type, not the category.
    EXPECT_TRUE(has_category(token_type::TYPE_INT, token_category::IDENTIFIER));
    EXPECT_TRUE(has_category(token_type::IDENTIFIER, token_category::IDENTIFIER));
    EXPECT_NE(token_type::TYPE_INT, token_type::IDENTIFIER);
}

TEST(TokenType, TokenTypeNameReturnsTheEnumeratorSpelling) {
    EXPECT_EQ(token_type_name(token_type::KEYWORD_DEF), "KEYWORD_DEF");
    EXPECT_EQ(token_type_name(token_type::TYPE_STRING), "TYPE_STRING");
    EXPECT_EQ(token_type_name(token_type::OP_DOUBLE_STAR_ASSIGN), "OP_DOUBLE_STAR_ASSIGN");
    EXPECT_EQ(token_type_name(token_type::ELLIPSIS), "ELLIPSIS");
    EXPECT_EQ(token_type_name(static_cast<token_type>(0xDEADBEEFu)), "UNKNOWN");
}

TEST(TokenType, EveryEnumeratorHasAName) {
    for (token_type type : ALL_TOKEN_TYPES) {
        EXPECT_NE(token_type_name(type), "UNKNOWN")
            << "missing name for raw value " << static_cast<uint32_t>(type);
    }
}

TEST(TokenCategory, IndentAndDedentAreSpecialOnly) {
    // Structure markers, not values: nothing in the stream binds to them, so
    // an "is this an object?" check must not pick them up.
    EXPECT_TRUE(has_category(token_type::INDENT, token_category::SPECIAL));
    EXPECT_TRUE(has_category(token_type::DEDENT, token_category::SPECIAL));
    EXPECT_FALSE(has_category(token_type::INDENT, token_category::OBJECT));
    EXPECT_FALSE(has_category(token_type::DEDENT, token_category::OBJECT));
    EXPECT_NE(token_type::INDENT, token_type::DEDENT);
}

TEST(TokenCategory, CompoundComparisonOperatorsAreKeywordAndOperator) {
    // Synthesised by the parser from two tokens; the scanner never emits
    // them. They live in the KEYWORD|OPERATOR space so the precedence and
    // comparison rules can dispatch on OPERATOR membership uniformly.
    EXPECT_TRUE(has_category(token_type::OP_NOT_IN, token_category::KEYWORD));
    EXPECT_TRUE(has_category(token_type::OP_NOT_IN, token_category::OPERATOR));
    EXPECT_TRUE(has_category(token_type::OP_IS_NOT, token_category::KEYWORD));
    EXPECT_TRUE(has_category(token_type::OP_IS_NOT, token_category::OPERATOR));
    EXPECT_NE(token_type::OP_NOT_IN, token_type::OP_IN);
    EXPECT_NE(token_type::OP_IS_NOT, token_type::OP_IS);
}

TEST(TokenEndPosition, SingleCharacterTokenEndsOneColumnLater) {
    const std::vector<Token> tokens = Lexer("x").tokenize();
    ASSERT_FALSE(tokens.empty());
    EXPECT_EQ(tokens[0].type(), token_type::IDENTIFIER);
    EXPECT_EQ(tokens[0].line_number(), 1);
    EXPECT_EQ(tokens[0].column_number(), 1);
    EXPECT_EQ(tokens[0].end_line(), 1);
    EXPECT_EQ(tokens[0].end_column(), 2);
}

TEST(TokenEndPosition, MultiCharacterTokenEndsPastItsLastCharacter) {
    const std::vector<Token> tokens = Lexer("total").tokenize();
    EXPECT_EQ(tokens[0].end_column(), 6);
}

TEST(TokenEndPosition, EndColumnCountsCharactersNotBytes) {
    // "é" is two UTF-8 bytes but one column, matching Lexer::advance().
    const std::vector<Token> tokens = Lexer("é").tokenize();
    EXPECT_EQ(tokens[0].type(), token_type::IDENTIFIER);
    EXPECT_EQ(tokens[0].end_column(), 2);
}

TEST(TokenEndPosition, TripleQuotedStringEndsOnALaterLine) {
    const std::vector<Token> tokens = Lexer("\"\"\"a\nb\"\"\"").tokenize();
    ASSERT_FALSE(tokens.empty());
    EXPECT_EQ(tokens[0].type(), token_type::LITERAL_STRING);
    EXPECT_EQ(tokens[0].line_number(), 1);
    EXPECT_EQ(tokens[0].end_line(), 2);
    EXPECT_EQ(tokens[0].end_column(), 5);
}

TEST(TokenEndPosition, SynthesizedEndOfFileTokenIsZeroWidth) {
    const std::vector<Token> tokens = Lexer("").tokenize();
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type(), token_type::TOKEN_EOF);
    EXPECT_EQ(tokens[0].line_number(), tokens[0].end_line());
    EXPECT_EQ(tokens[0].column_number(), tokens[0].end_column());
}

} // namespace
} // namespace cythonpp::domain::lexer
