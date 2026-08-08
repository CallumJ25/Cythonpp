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
    // Marks a token that denotes a Python *value* -- something that is an
    // instance of a class and therefore carries a runtime type and its
    // methods. Literals, True/False/None/Ellipsis, names (which bind to
    // objects) and builtin type names (a class is itself an object) all
    // carry it. Pure syntax does not: statement keywords, operators,
    // delimiters, punctuation, comments and the stream markers.
    //
    // The rule is "this token, standing alone, denotes a value", not "the
    // construct it introduces evaluates to a value". So KEYWORD_LAMBDA and
    // OPEN_BRACKET are not OBJECT even though `lambda: 1` and `[1, 2]`
    // produce objects -- without that restriction OBJECT would leak into
    // half the delimiters and stop meaning anything.
    OBJECT      = 1 << 8,
};

} // namespace cythonpp::domain::lexer

#endif // CYTHONPP_DOMAIN_LEXER_TOKEN_CATEGORY_H
