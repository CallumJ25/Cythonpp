#include "domain/codegen/name_mangler.h"

namespace cythonpp::domain::codegen {
namespace {

bool is_ascii_letter_or_underscore(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

bool is_ascii_digit(char c) { return c >= '0' && c <= '9'; }

} // namespace

bool is_manglable_identifier(const std::string& identifier) {
    if (identifier.empty()) {
        return false;
    }
    if (!is_ascii_letter_or_underscore(identifier.front())) {
        return false;
    }
    for (const char c : identifier) {
        if (!is_ascii_letter_or_underscore(c) && !is_ascii_digit(c)) {
            return false;
        }
    }
    return true;
}

std::string mangle(const std::string& identifier) { return "cy_" + identifier; }

} // namespace cythonpp::domain::codegen
