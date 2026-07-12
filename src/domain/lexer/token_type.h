#ifndef CYTHONPP_DOMAIN_LEXER_TOKEN_TYPE_H
#define CYTHONPP_DOMAIN_LEXER_TOKEN_TYPE_H

#include <cstdint>

#include "token_category.h"

namespace cythonpp::domain::lexer {

namespace detail {

constexpr uint16_t cat(token_category category) {
    return static_cast<uint16_t>(category);
}

// Packs category flag bits into the high byte and a per-category subtype
// index into the low byte, so category_of() can recover the flags with a
// single shift+mask instead of a lookup table.
constexpr uint16_t make_token(uint16_t category_flags, uint16_t subtype) {
    return static_cast<uint16_t>((category_flags << 8) | subtype);
}

} // namespace detail

enum class token_type : uint16_t {
    // Special
    TOKEN_EOF   = detail::make_token(detail::cat(token_category::SPECIAL), 0),
    TOKEN_ERROR = detail::make_token(detail::cat(token_category::SPECIAL), 1),
    NEWLINE     = detail::make_token(detail::cat(token_category::SPECIAL), 2),
    SPACE       = detail::make_token(detail::cat(token_category::SPECIAL), 3),
    TAB         = detail::make_token(detail::cat(token_category::SPECIAL), 4),

    // Identifiers
    IDENTIFIER = detail::make_token(detail::cat(token_category::IDENTIFIER), 0),

    // String literals
    LITERAL_STRING = detail::make_token(detail::cat(token_category::STRING), 0),

    // Number literals
    LITERAL_INT   = detail::make_token(detail::cat(token_category::NUMBER), 0),
    LITERAL_FLOAT = detail::make_token(detail::cat(token_category::NUMBER), 1),

    // Keywords
    KEYWORD_IF     = detail::make_token(detail::cat(token_category::KEYWORD), 0),
    KEYWORD_ELSE   = detail::make_token(detail::cat(token_category::KEYWORD), 1),
    KEYWORD_FOR    = detail::make_token(detail::cat(token_category::KEYWORD), 2),
    KEYWORD_ELIF   = detail::make_token(detail::cat(token_category::KEYWORD), 3),
    KEYWORD_WHILE  = detail::make_token(detail::cat(token_category::KEYWORD), 4),
    KEYWORD_RETURN = detail::make_token(detail::cat(token_category::KEYWORD), 5),
    KEYWORD_DEF    = detail::make_token(detail::cat(token_category::KEYWORD), 6),
    KEYWORD_CLASS  = detail::make_token(detail::cat(token_category::KEYWORD), 7),

    // Bools: reserved words that are also numerically int subtypes (True == 1).
    BOOL_TRUE  = detail::make_token(detail::cat(token_category::KEYWORD) | detail::cat(token_category::NUMBER), 0),
    BOOL_FALSE = detail::make_token(detail::cat(token_category::KEYWORD) | detail::cat(token_category::NUMBER), 1),

    // Builtin type names used in annotations: reserved in this context, but
    // lexically identifier-shaped.
    TYPE_INT    = detail::make_token(detail::cat(token_category::KEYWORD) | detail::cat(token_category::IDENTIFIER), 0),
    TYPE_FLOAT  = detail::make_token(detail::cat(token_category::KEYWORD) | detail::cat(token_category::IDENTIFIER), 1),
    TYPE_STRING = detail::make_token(detail::cat(token_category::KEYWORD) | detail::cat(token_category::IDENTIFIER), 2),
    TYPE_BOOL   = detail::make_token(detail::cat(token_category::KEYWORD) | detail::cat(token_category::IDENTIFIER), 3),
    TYPE_LIST   = detail::make_token(detail::cat(token_category::KEYWORD) | detail::cat(token_category::IDENTIFIER), 4),
    TYPE_DICT   = detail::make_token(detail::cat(token_category::KEYWORD) | detail::cat(token_category::IDENTIFIER), 5),
    TYPE_SET    = detail::make_token(detail::cat(token_category::KEYWORD) | detail::cat(token_category::IDENTIFIER), 6),
    TYPE_TUPLE  = detail::make_token(detail::cat(token_category::KEYWORD) | detail::cat(token_category::IDENTIFIER), 7),

    // Operators
    OP_ASSIGN        = detail::make_token(detail::cat(token_category::OPERATOR), 0),
    OP_PLUS          = detail::make_token(detail::cat(token_category::OPERATOR), 1),
    OP_MINUS         = detail::make_token(detail::cat(token_category::OPERATOR), 2),
    OP_EQUAL         = detail::make_token(detail::cat(token_category::OPERATOR), 3),
    OP_NOT_EQUAL     = detail::make_token(detail::cat(token_category::OPERATOR), 4),
    OP_LESS          = detail::make_token(detail::cat(token_category::OPERATOR), 5),
    OP_GREATER       = detail::make_token(detail::cat(token_category::OPERATOR), 6),
    OP_LESS_EQUAL    = detail::make_token(detail::cat(token_category::OPERATOR), 7),
    OP_GREATER_EQUAL = detail::make_token(detail::cat(token_category::OPERATOR), 8),
    OP_AND           = detail::make_token(detail::cat(token_category::OPERATOR), 9),
    OP_OR            = detail::make_token(detail::cat(token_category::OPERATOR), 10),
    OP_NOT           = detail::make_token(detail::cat(token_category::OPERATOR), 11),

    // Delimiters
    OPEN_PAREN    = detail::make_token(detail::cat(token_category::DELIMITER), 0),
    CLOSE_PAREN   = detail::make_token(detail::cat(token_category::DELIMITER), 1),
    OPEN_BRACE    = detail::make_token(detail::cat(token_category::DELIMITER), 2),
    CLOSE_BRACE   = detail::make_token(detail::cat(token_category::DELIMITER), 3),
    OPEN_BRACKET  = detail::make_token(detail::cat(token_category::DELIMITER), 4),
    CLOSE_BRACKET = detail::make_token(detail::cat(token_category::DELIMITER), 5),

    // Punctuation (incl. comments)
    SEMICOLON      = detail::make_token(detail::cat(token_category::PUNCTUATION), 0),
    COMMA          = detail::make_token(detail::cat(token_category::PUNCTUATION), 1),
    DOT            = detail::make_token(detail::cat(token_category::PUNCTUATION), 2),
    COLON          = detail::make_token(detail::cat(token_category::PUNCTUATION), 3),
    QUESTION       = detail::make_token(detail::cat(token_category::PUNCTUATION), 4),
    COMMENT_SINGLE = detail::make_token(detail::cat(token_category::PUNCTUATION), 5),
    COMMENT_MULTI  = detail::make_token(detail::cat(token_category::PUNCTUATION), 6),
};

// Recovers the raw category flag bits packed into the high byte of a
// token_type. May contain more than one flag set.
constexpr uint16_t category_of(token_type type) {
    return (static_cast<uint16_t>(type) >> 8) & 0xFF;
}

// True if `type` carries the given category flag (a token_type may carry
// more than one, e.g. BOOL_TRUE is both KEYWORD and NUMBER).
constexpr bool has_category(token_type type, token_category category) {
    return (category_of(type) & detail::cat(category)) != 0;
}

} // namespace cythonpp::domain::lexer

#endif // CYTHONPP_DOMAIN_LEXER_TOKEN_TYPE_H
