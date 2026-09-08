#include "literal_type.h"

#include <cstddef>
#include <string>

namespace cythonpp::domain::semantic {

Type literal_type(lexer::token_type type) {
    switch (type) {
    case lexer::token_type::LITERAL_INT:
        return Type::int_();
    case lexer::token_type::LITERAL_FLOAT:
        return Type::float_();
    case lexer::token_type::LITERAL_COMPLEX:
        return Type::complex_();
    case lexer::token_type::LITERAL_STRING:
        return Type::str();
    case lexer::token_type::LITERAL_BYTES:
        return Type::bytes();
    case lexer::token_type::BOOL_TRUE:
    case lexer::token_type::BOOL_FALSE:
        return Type::bool_();
    case lexer::token_type::KEYWORD_NONE:
        return Type::none();
    case lexer::token_type::ELLIPSIS:
        return Type::ellipsis();
    default:
        // Every token type ast::Constant can carry is listed above. A default
        // is right here, unlike in a switch over TypeKind: token_type has
        // well over a hundred enumerators and all but these nine are not
        // literals at all.
        return Type::unknown();
    }
}

bool integer_literal_fits_64_bits(const std::string& lexeme, bool allow_two_to_63) {
    // 2^63. See the header for why this is the bound and not 2^63 - 1, and for
    // when a caller passes allow_two_to_63 = false to tighten it by one.
    constexpr unsigned long long LIMIT_INCLUSIVE = 9223372036854775808ULL;
    const unsigned long long LIMIT = allow_two_to_63 ? LIMIT_INCLUSIVE : LIMIT_INCLUSIVE - 1;

    std::string digits;
    for (const char character : lexeme) {
        if (character != '_') {
            digits.push_back(character);
        }
    }

    unsigned long long base = 10;
    std::size_t index = 0;
    if (digits.size() >= 2 && digits[0] == '0') {
        switch (digits[1]) {
        case 'x':
        case 'X':
            base = 16;
            index = 2;
            break;
        case 'o':
        case 'O':
            base = 8;
            index = 2;
            break;
        case 'b':
        case 'B':
            base = 2;
            index = 2;
            break;
        default:
            break;
        }
    }

    unsigned long long value = 0;
    for (; index < digits.size(); ++index) {
        const char character = digits[index];
        unsigned long long digit = 0;
        if (character >= '0' && character <= '9') {
            digit = static_cast<unsigned long long>(character - '0');
        } else if (character >= 'a' && character <= 'f') {
            digit = static_cast<unsigned long long>(character - 'a') + 10;
        } else if (character >= 'A' && character <= 'F') {
            digit = static_cast<unsigned long long>(character - 'A') + 10;
        } else {
            // Not a digit this function can read -- an exponent, a suffix, or
            // an empty lexeme. Nothing to validate, and a false report is
            // worse than no report.
            return true;
        }

        // The exact overflow test, rather than value * base + digit > LIMIT,
        // which would itself overflow before the comparison ran.
        if (value > LIMIT / base) {
            return false;
        }
        if (value == LIMIT / base && digit > LIMIT % base) {
            return false;
        }
        value = value * base + digit;
    }
    return true;
}

} // namespace cythonpp::domain::semantic
