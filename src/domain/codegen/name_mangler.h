#ifndef CYTHONPP_DOMAIN_CODEGEN_NAME_MANGLER_H
#define CYTHONPP_DOMAIN_CODEGEN_NAME_MANGLER_H

#include <string>

namespace cythonpp::domain::codegen {

// Whether `identifier` is one this stage can mangle: ASCII, starting with a
// letter or underscore. Non-ASCII identifiers are legal Python and the lexer
// preserves their bytes exactly, but C++'s identifier rules for them are
// different enough that emitting one would be a guess; the caller refuses
// instead.
bool is_manglable_identifier(const std::string& identifier);

// Python identifier to C++ identifier.
//
// The whole C++-keyword problem disappears in one rule: prefix everything.
// No C++ keyword begins with "cy_", so no mangled name can collide with one,
// and prefixing is INJECTIVE -- Python "cy_x" becomes "cy_cy_x" and stays
// distinct from Python "x". Injectivity is the correctness requirement here;
// a collision would be a silent miscompile, not a readability wart.
//
// Callers must check is_manglable_identifier first. This function assumes it.
std::string mangle(const std::string& identifier);

} // namespace cythonpp::domain::codegen

#endif // CYTHONPP_DOMAIN_CODEGEN_NAME_MANGLER_H
