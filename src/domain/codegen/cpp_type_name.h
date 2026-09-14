#ifndef CYTHONPP_DOMAIN_CODEGEN_CPP_TYPE_NAME_H
#define CYTHONPP_DOMAIN_CODEGEN_CPP_TYPE_NAME_H

#include <optional>
#include <string>

#include "domain/semantic/type.h"

namespace cythonpp::domain::codegen {

// The C++ spelling of a Type, or nullopt when this slice cannot represent it.
//
// TOTAL AND CONSERVATIVE: nullopt is the answer for everything outside the
// scalar surface, including Unknown and every Union. That single fact is what
// makes several refusals fall out for free rather than needing their own
// checks -- `and`/`or` over differing operand types produces a Union, so the
// emitter refuses it here without a rule of its own.
std::optional<std::string> cpp_type_name(const semantic::Type& type);

} // namespace cythonpp::domain::codegen

#endif // CYTHONPP_DOMAIN_CODEGEN_CPP_TYPE_NAME_H
