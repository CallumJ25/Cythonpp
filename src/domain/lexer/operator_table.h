#ifndef CYTHONPP_DOMAIN_LEXER_OPERATOR_TABLE_H
#define CYTHONPP_DOMAIN_LEXER_OPERATOR_TABLE_H

#include <cstddef>
#include <string_view>

#include "token_type.h"

namespace cythonpp::domain::lexer {

// What a maximal-munch lookup found. `length` is how many source characters
// the token consumed; 0 means nothing matched, and `type` is then
// TOKEN_ERROR.
struct OperatorMatch {
    token_type  type;
    std::size_t length;
};

// Maximal munch: given source text starting at a punctuation character,
// returns the longest operator, delimiter or punctuation token that
// prefixes it, so '**=' beats '**' beats '*'. Never reads past the end of
// `text`, so a '*' at end-of-file matches OP_STAR rather than running off
// the buffer.
OperatorMatch longest_operator_at(std::string_view text);

// True if `c` can begin any entry in the table, letting the scanner dispatch
// into longest_operator_at() without a chain of character comparisons. Note
// '!' is true because of '!=', even though a lone '!' is not a Python token
// -- longest_operator_at() then returns length 0 and the scanner reports the
// error, which is the behaviour we want.
bool is_operator_start(char c);

// Reverse lookup, for diagnostics. Empty view if `type` is not a
// punctuation-spelled token.
std::string_view operator_lexeme_of(token_type type);

} // namespace cythonpp::domain::lexer

#endif // CYTHONPP_DOMAIN_LEXER_OPERATOR_TABLE_H
