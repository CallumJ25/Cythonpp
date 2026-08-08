#ifndef CYTHONPP_DOMAIN_LEXER_TOKEN_TYPE_NAME_H
#define CYTHONPP_DOMAIN_LEXER_TOKEN_TYPE_NAME_H

#include <string_view>

#include "token_type.h"

namespace cythonpp::domain::lexer {

// The enumerator spelling of a token_type, for debug dumps and diagnostics.
// Returns a view of a string literal, so there is no allocation and the
// result outlives any caller. Yields "UNKNOWN" for a value that is not a
// declared enumerator.
//
// This lives in the domain rather than in the CLI because the name of a
// token type is a property of the token model, and the parser's error
// messages will want it too. It is a separate file from token_type.h so
// that header stays header-only and constexpr.
std::string_view token_type_name(token_type type);

} // namespace cythonpp::domain::lexer

#endif // CYTHONPP_DOMAIN_LEXER_TOKEN_TYPE_NAME_H
