#ifndef CYTHONPP_DOMAIN_LEXER_KEYWORD_TABLE_H
#define CYTHONPP_DOMAIN_LEXER_KEYWORD_TABLE_H

#include <optional>
#include <string_view>

#include "token_type.h"

namespace cythonpp::domain::lexer {

// Python's 35 reserved words. These can never be rebound, so the scanner
// classifies them with no context at all. Returns nullopt for anything
// else, including builtin type names and soft keywords.
std::optional<token_type> reserved_keyword_of(std::string_view word);

// Builtin type names (int, str, list, ...). These are *not* reserved --
// `int = 5` is legal Python -- so they only become TYPE_* when the scanner
// is in annotation position (after ':' in a declaration or parameter, after
// '->', or inside the subscript of a generic like list[int]). Kept as a
// separate query from reserved_keyword_of() precisely so the scanner can
// consult it conditionally on that state.
std::optional<token_type> builtin_type_of(std::string_view word);

// Soft keywords (match, case, _): reserved only where the grammar expects
// them, which needs statement context the scanner does not have. The
// scanner always emits IDENTIFIER; the parser re-classifies through this.
std::optional<token_type> soft_keyword_of(std::string_view word);

// The scanner's whole word path in one call. Reserved words win
// unconditionally; builtin type names apply only when `in_annotation`;
// everything else, including every soft keyword, is IDENTIFIER. Never
// fails -- an unknown or empty word yields IDENTIFIER.
token_type classify_word(std::string_view word, bool in_annotation);

// Reverse lookup, for diagnostics such as "expected 'else'". Empty view if
// `type` is not a word-shaped token.
std::string_view keyword_lexeme_of(token_type type);

} // namespace cythonpp::domain::lexer

#endif // CYTHONPP_DOMAIN_LEXER_KEYWORD_TABLE_H
