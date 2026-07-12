#ifndef CYTHONPP_DOMAIN_LEXER_TOKEN_CATEGORY_H
#define CYTHONPP_DOMAIN_LEXER_TOKEN_CATEGORY_H

#include <cstdint>

namespace cythonpp::domain::lexer {

// Each category is an independent bit so a token_type can belong to more
// than one category at once (e.g. BOOL_TRUE is both KEYWORD and NUMBER).
enum class token_category : uint16_t {
    NUMBER      = 1 << 0,
    STRING      = 1 << 1,
    KEYWORD     = 1 << 2,
    OPERATOR    = 1 << 3,
    DELIMITER   = 1 << 4,
    PUNCTUATION = 1 << 5,
    IDENTIFIER  = 1 << 6,
    SPECIAL     = 1 << 7,
};

} // namespace cythonpp::domain::lexer

#endif // CYTHONPP_DOMAIN_LEXER_TOKEN_CATEGORY_H
