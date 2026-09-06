#ifndef CYTHONPP_DOMAIN_LEXER_TOKEN_TYPE_H
#define CYTHONPP_DOMAIN_LEXER_TOKEN_TYPE_H

#include <cstdint>

#include "token_category.h"

namespace cythonpp::domain::lexer {

namespace detail {

constexpr uint32_t cat(token_category category) {
    return static_cast<uint32_t>(category);
}

// Packs category flag bits into the high 16 bits and a per-combination
// subtype index into the low 16 bits, so category_of() can recover the
// flags with a single shift+mask instead of a lookup table.
//
// Sixteen flag bits are needed rather than eight because OBJECT is bit 8:
// under an 8-bit split it would shift clean out of a uint16_t and yield
// category 0, silently aliasing every value-bearing token onto every other.
constexpr uint32_t make_token(uint32_t category_flags, uint32_t subtype) {
    return (category_flags << 16) | subtype;
}

// One name per distinct flag combination. Subtype indices restart at 0 for
// each of these, so every constant below owns its own numbering space --
// reusing a constant is the only way to join a space, which keeps "what is
// the next free index?" answerable by reading one contiguous block.
//
// Uniqueness within a space is the requirement, not density: gaps are
// harmless, so a token can later move between spaces without renumbering
// anything else.
inline constexpr uint32_t SPECIAL_FLAGS     = cat(token_category::SPECIAL);
inline constexpr uint32_t NAME_FLAGS        = cat(token_category::IDENTIFIER) | cat(token_category::OBJECT);
inline constexpr uint32_t STRING_FLAGS      = cat(token_category::STRING) | cat(token_category::OBJECT);
inline constexpr uint32_t NUMBER_FLAGS      = cat(token_category::NUMBER) | cat(token_category::OBJECT);
inline constexpr uint32_t BOOL_FLAGS        = cat(token_category::KEYWORD) | cat(token_category::NUMBER) | cat(token_category::OBJECT);
inline constexpr uint32_t NONE_FLAGS        = cat(token_category::KEYWORD) | cat(token_category::OBJECT);
inline constexpr uint32_t VALUE_FLAGS       = cat(token_category::OBJECT);
inline constexpr uint32_t KEYWORD_FLAGS     = cat(token_category::KEYWORD);
inline constexpr uint32_t KEYWORD_OP_FLAGS  = cat(token_category::KEYWORD) | cat(token_category::OPERATOR);
inline constexpr uint32_t OPERATOR_FLAGS    = cat(token_category::OPERATOR);
inline constexpr uint32_t DELIMITER_FLAGS   = cat(token_category::DELIMITER);
inline constexpr uint32_t PUNCTUATION_FLAGS = cat(token_category::PUNCTUATION);
inline constexpr uint32_t TYPE_NAME_FLAGS   = cat(token_category::KEYWORD) | cat(token_category::IDENTIFIER) | cat(token_category::OBJECT);

} // namespace detail

enum class token_type : uint32_t {
    // Special: stream markers, never a Python value.
    //
    // SPACE/TAB are what the scanner emits, one per leading character.
    // INDENT/DEDENT are what IndentationPass replaces them with, so the two
    // pairs never coexist in the same stream: a stream that has been through
    // the pass has no SPACE or TAB in it at all.
    TOKEN_EOF   = detail::make_token(detail::SPECIAL_FLAGS, 0),
    TOKEN_ERROR = detail::make_token(detail::SPECIAL_FLAGS, 1),
    NEWLINE     = detail::make_token(detail::SPECIAL_FLAGS, 2),
    SPACE       = detail::make_token(detail::SPECIAL_FLAGS, 3),
    TAB         = detail::make_token(detail::SPECIAL_FLAGS, 4),
    INDENT      = detail::make_token(detail::SPECIAL_FLAGS, 5),
    DEDENT      = detail::make_token(detail::SPECIAL_FLAGS, 6),

    // Identifiers: a name denotes whatever object it is bound to.
    IDENTIFIER = detail::make_token(detail::NAME_FLAGS, 0),

    // String literals. An f-string is lexed into parts so its interpolated
    // expressions become real tokens carrying real file positions.
    LITERAL_STRING  = detail::make_token(detail::STRING_FLAGS, 0),
    LITERAL_BYTES   = detail::make_token(detail::STRING_FLAGS, 1),
    FSTRING_START   = detail::make_token(detail::STRING_FLAGS, 2),
    FSTRING_MIDDLE  = detail::make_token(detail::STRING_FLAGS, 3),
    FSTRING_END     = detail::make_token(detail::STRING_FLAGS, 4),

    // Number literals.
    LITERAL_INT     = detail::make_token(detail::NUMBER_FLAGS, 0),
    LITERAL_FLOAT   = detail::make_token(detail::NUMBER_FLAGS, 1),
    LITERAL_COMPLEX = detail::make_token(detail::NUMBER_FLAGS, 2),

    // Bools: reserved words that are also numerically int subtypes
    // (True == 1) and are bool instances, hence OBJECT.
    BOOL_TRUE  = detail::make_token(detail::BOOL_FLAGS, 0),
    BOOL_FALSE = detail::make_token(detail::BOOL_FLAGS, 1),

    // None: a reserved word denoting the NoneType singleton. There is
    // deliberately no separate TYPE_NONE -- None is reserved and can never
    // lex as anything else, so the parser reads this as the type when it
    // appears in annotation position.
    KEYWORD_NONE = detail::make_token(detail::NONE_FLAGS, 0),

    // Ellipsis: spelled with punctuation characters but grammatically an
    // atom denoting the Ellipsis singleton, so it is OBJECT rather than
    // PUNCTUATION -- PUNCTUATION here means "separator carrying no value".
    ELLIPSIS = detail::make_token(detail::VALUE_FLAGS, 0),

    // Reserved words with a statement- or binding-level role.
    KEYWORD_IF       = detail::make_token(detail::KEYWORD_FLAGS, 0),
    KEYWORD_ELSE     = detail::make_token(detail::KEYWORD_FLAGS, 1),
    KEYWORD_FOR      = detail::make_token(detail::KEYWORD_FLAGS, 2),
    KEYWORD_ELIF     = detail::make_token(detail::KEYWORD_FLAGS, 3),
    KEYWORD_WHILE    = detail::make_token(detail::KEYWORD_FLAGS, 4),
    KEYWORD_RETURN   = detail::make_token(detail::KEYWORD_FLAGS, 5),
    KEYWORD_DEF      = detail::make_token(detail::KEYWORD_FLAGS, 6),
    KEYWORD_CLASS    = detail::make_token(detail::KEYWORD_FLAGS, 7),
    KEYWORD_AS       = detail::make_token(detail::KEYWORD_FLAGS, 8),
    KEYWORD_ASSERT   = detail::make_token(detail::KEYWORD_FLAGS, 9),
    KEYWORD_ASYNC    = detail::make_token(detail::KEYWORD_FLAGS, 10),
    KEYWORD_AWAIT    = detail::make_token(detail::KEYWORD_FLAGS, 11),
    KEYWORD_BREAK    = detail::make_token(detail::KEYWORD_FLAGS, 12),
    KEYWORD_CONTINUE = detail::make_token(detail::KEYWORD_FLAGS, 13),
    KEYWORD_DEL      = detail::make_token(detail::KEYWORD_FLAGS, 14),
    KEYWORD_EXCEPT   = detail::make_token(detail::KEYWORD_FLAGS, 15),
    KEYWORD_FINALLY  = detail::make_token(detail::KEYWORD_FLAGS, 16),
    KEYWORD_FROM     = detail::make_token(detail::KEYWORD_FLAGS, 17),
    KEYWORD_GLOBAL   = detail::make_token(detail::KEYWORD_FLAGS, 18),
    KEYWORD_IMPORT   = detail::make_token(detail::KEYWORD_FLAGS, 19),
    KEYWORD_LAMBDA   = detail::make_token(detail::KEYWORD_FLAGS, 20),
    KEYWORD_NONLOCAL = detail::make_token(detail::KEYWORD_FLAGS, 21),
    KEYWORD_PASS     = detail::make_token(detail::KEYWORD_FLAGS, 22),
    KEYWORD_RAISE    = detail::make_token(detail::KEYWORD_FLAGS, 23),
    KEYWORD_TRY      = detail::make_token(detail::KEYWORD_FLAGS, 24),
    KEYWORD_WITH     = detail::make_token(detail::KEYWORD_FLAGS, 25),
    KEYWORD_YIELD    = detail::make_token(detail::KEYWORD_FLAGS, 26),

    // Soft keywords: reserved only where the grammar expects them, which
    // needs statement context the scanner does not have. The scanner always
    // emits IDENTIFIER for these words; the parser re-classifies through
    // soft_keyword_of(). Declared here so that costs no renumbering.
    KEYWORD_MATCH      = detail::make_token(detail::KEYWORD_FLAGS, 27),
    KEYWORD_CASE       = detail::make_token(detail::KEYWORD_FLAGS, 28),
    KEYWORD_UNDERSCORE = detail::make_token(detail::KEYWORD_FLAGS, 29),

    // Reserved words that are *only* ever expression operators. KEYWORD
    // because the scanner finds them on the word path and they can never be
    // rebound; OPERATOR because the parser's precedence table drives off
    // OPERATOR membership. This dual membership is why the flag scheme
    // exists. `if`/`else` (ternary) and `lambda` deliberately do not
    // qualify -- they have statement-level roles, and tagging them OPERATOR
    // would poison every "is this an operator?" check.
    OP_AND = detail::make_token(detail::KEYWORD_OP_FLAGS, 0),
    OP_OR  = detail::make_token(detail::KEYWORD_OP_FLAGS, 1),
    OP_NOT = detail::make_token(detail::KEYWORD_OP_FLAGS, 2),
    OP_IS  = detail::make_token(detail::KEYWORD_OP_FLAGS, 3),
    OP_IN  = detail::make_token(detail::KEYWORD_OP_FLAGS, 4),

    // Two-word comparison operators. The scanner never emits these -- `a not
    // in b` arrives as OP_NOT then OP_IN -- but Compare::Rest names each
    // comparison with a single token_type, so the parser folds the pair here.
    // Declared alongside the words they are built from so the numbering stays
    // in one contiguous block.
    OP_NOT_IN = detail::make_token(detail::KEYWORD_OP_FLAGS, 5),
    OP_IS_NOT = detail::make_token(detail::KEYWORD_OP_FLAGS, 6),

    // Punctuation-spelled operators. Indices 9-11 previously held
    // OP_AND/OP_OR/OP_NOT, which moved to the KEYWORD|OPERATOR space above;
    // reusing them is safe because token_type values are never persisted
    // outside the process.
    OP_ASSIGN              = detail::make_token(detail::OPERATOR_FLAGS, 0),
    OP_PLUS                = detail::make_token(detail::OPERATOR_FLAGS, 1),
    OP_MINUS               = detail::make_token(detail::OPERATOR_FLAGS, 2),
    OP_EQUAL               = detail::make_token(detail::OPERATOR_FLAGS, 3),
    OP_NOT_EQUAL           = detail::make_token(detail::OPERATOR_FLAGS, 4),
    OP_LESS                = detail::make_token(detail::OPERATOR_FLAGS, 5),
    OP_GREATER             = detail::make_token(detail::OPERATOR_FLAGS, 6),
    OP_LESS_EQUAL          = detail::make_token(detail::OPERATOR_FLAGS, 7),
    OP_GREATER_EQUAL       = detail::make_token(detail::OPERATOR_FLAGS, 8),
    OP_STAR                = detail::make_token(detail::OPERATOR_FLAGS, 9),
    OP_SLASH               = detail::make_token(detail::OPERATOR_FLAGS, 10),
    OP_DOUBLE_SLASH        = detail::make_token(detail::OPERATOR_FLAGS, 11),
    OP_PERCENT             = detail::make_token(detail::OPERATOR_FLAGS, 12),
    OP_DOUBLE_STAR         = detail::make_token(detail::OPERATOR_FLAGS, 13),
    // '@' is both matrix-multiply and the decorator marker; one token,
    // disambiguated by the parser from statement position.
    OP_AT                  = detail::make_token(detail::OPERATOR_FLAGS, 14),
    OP_AMPERSAND           = detail::make_token(detail::OPERATOR_FLAGS, 15),
    OP_PIPE                = detail::make_token(detail::OPERATOR_FLAGS, 16),
    OP_CARET               = detail::make_token(detail::OPERATOR_FLAGS, 17),
    OP_TILDE               = detail::make_token(detail::OPERATOR_FLAGS, 18),
    OP_LEFT_SHIFT          = detail::make_token(detail::OPERATOR_FLAGS, 19),
    OP_RIGHT_SHIFT         = detail::make_token(detail::OPERATOR_FLAGS, 20),
    OP_PLUS_ASSIGN         = detail::make_token(detail::OPERATOR_FLAGS, 21),
    OP_MINUS_ASSIGN        = detail::make_token(detail::OPERATOR_FLAGS, 22),
    OP_STAR_ASSIGN         = detail::make_token(detail::OPERATOR_FLAGS, 23),
    OP_SLASH_ASSIGN        = detail::make_token(detail::OPERATOR_FLAGS, 24),
    OP_DOUBLE_SLASH_ASSIGN = detail::make_token(detail::OPERATOR_FLAGS, 25),
    OP_PERCENT_ASSIGN      = detail::make_token(detail::OPERATOR_FLAGS, 26),
    OP_DOUBLE_STAR_ASSIGN  = detail::make_token(detail::OPERATOR_FLAGS, 27),
    OP_AT_ASSIGN           = detail::make_token(detail::OPERATOR_FLAGS, 28),
    OP_AMPERSAND_ASSIGN    = detail::make_token(detail::OPERATOR_FLAGS, 29),
    OP_PIPE_ASSIGN         = detail::make_token(detail::OPERATOR_FLAGS, 30),
    OP_CARET_ASSIGN        = detail::make_token(detail::OPERATOR_FLAGS, 31),
    OP_RIGHT_SHIFT_ASSIGN  = detail::make_token(detail::OPERATOR_FLAGS, 32),
    OP_LEFT_SHIFT_ASSIGN   = detail::make_token(detail::OPERATOR_FLAGS, 33),
    OP_WALRUS              = detail::make_token(detail::OPERATOR_FLAGS, 34),
    OP_ARROW               = detail::make_token(detail::OPERATOR_FLAGS, 35),

    // Delimiters.
    OPEN_PAREN    = detail::make_token(detail::DELIMITER_FLAGS, 0),
    CLOSE_PAREN   = detail::make_token(detail::DELIMITER_FLAGS, 1),
    OPEN_BRACE    = detail::make_token(detail::DELIMITER_FLAGS, 2),
    CLOSE_BRACE   = detail::make_token(detail::DELIMITER_FLAGS, 3),
    OPEN_BRACKET  = detail::make_token(detail::DELIMITER_FLAGS, 4),
    CLOSE_BRACKET = detail::make_token(detail::DELIMITER_FLAGS, 5),

    // Punctuation: structural separators that carry no value. QUESTION and
    // COMMENT_MULTI have no Python spelling -- '?' is not a Python token and
    // Python has no block comment, a docstring being an ordinary
    // LITERAL_STRING. Both are unreachable from the operator table and the
    // scanner never emits them; they are kept only for source compatibility
    // and should be removed in a follow-up.
    SEMICOLON      = detail::make_token(detail::PUNCTUATION_FLAGS, 0),
    COMMA          = detail::make_token(detail::PUNCTUATION_FLAGS, 1),
    DOT            = detail::make_token(detail::PUNCTUATION_FLAGS, 2),
    COLON          = detail::make_token(detail::PUNCTUATION_FLAGS, 3),
    QUESTION       = detail::make_token(detail::PUNCTUATION_FLAGS, 4),
    COMMENT_SINGLE = detail::make_token(detail::PUNCTUATION_FLAGS, 5),
    COMMENT_MULTI  = detail::make_token(detail::PUNCTUATION_FLAGS, 6),

    // Builtin type names. Unlike the reserved words these are ordinary
    // identifiers in Python (`int = 5` is legal), so the scanner only
    // produces them in annotation position -- hence they carry both KEYWORD
    // and IDENTIFIER. OBJECT because a class is itself an object.
    TYPE_INT       = detail::make_token(detail::TYPE_NAME_FLAGS, 0),
    TYPE_FLOAT     = detail::make_token(detail::TYPE_NAME_FLAGS, 1),
    TYPE_STRING    = detail::make_token(detail::TYPE_NAME_FLAGS, 2),
    TYPE_BOOL      = detail::make_token(detail::TYPE_NAME_FLAGS, 3),
    TYPE_LIST      = detail::make_token(detail::TYPE_NAME_FLAGS, 4),
    TYPE_DICT      = detail::make_token(detail::TYPE_NAME_FLAGS, 5),
    TYPE_SET       = detail::make_token(detail::TYPE_NAME_FLAGS, 6),
    TYPE_TUPLE     = detail::make_token(detail::TYPE_NAME_FLAGS, 7),
    TYPE_BYTES     = detail::make_token(detail::TYPE_NAME_FLAGS, 8),
    TYPE_COMPLEX   = detail::make_token(detail::TYPE_NAME_FLAGS, 9),
    TYPE_FROZENSET = detail::make_token(detail::TYPE_NAME_FLAGS, 10),
    TYPE_BYTEARRAY = detail::make_token(detail::TYPE_NAME_FLAGS, 11),
    // The builtin class `object`, unrelated to token_category::OBJECT.
    TYPE_OBJECT    = detail::make_token(detail::TYPE_NAME_FLAGS, 12),

    // When adding an enumerator: pick the right flag combination, take the
    // next free subtype index *within that exact combination*, and add it to
    // ALL_TOKEN_TYPES in tests/domain/lexer/token_test.cpp -- C++17 has no
    // enum reflection, so that hand-maintained list is the only thing
    // guarding against two enumerators silently sharing a value.
};

// Recovers the raw category flag bits packed into the high 16 bits of a
// token_type. May contain more than one flag set.
constexpr uint32_t category_of(token_type type) {
    return (static_cast<uint32_t>(type) >> 16) & 0xFFFFu;
}

// Recovers the per-combination subtype index from the low 16 bits. Subtype
// indices restart at 0 for every distinct flag combination, so this value is
// only meaningful alongside category_of().
constexpr uint32_t subtype_of(token_type type) {
    return static_cast<uint32_t>(type) & 0xFFFFu;
}

// True if `type` carries the given category flag (a token_type may carry
// more than one, e.g. BOOL_TRUE is KEYWORD, NUMBER and OBJECT).
constexpr bool has_category(token_type type, token_category category) {
    return (category_of(type) & detail::cat(category)) != 0;
}

} // namespace cythonpp::domain::lexer

#endif // CYTHONPP_DOMAIN_LEXER_TOKEN_TYPE_H
