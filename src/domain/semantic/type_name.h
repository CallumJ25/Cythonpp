#ifndef CYTHONPP_DOMAIN_SEMANTIC_TYPE_NAME_H
#define CYTHONPP_DOMAIN_SEMANTIC_TYPE_NAME_H

#include <string>

#include "type.h"

namespace cythonpp::domain::semantic {

// `type` as a display string, for diagnostics and for test assertions.
//
// Modelled on mypy's spelling -- `list[int]`, `dict[str, int]`, `str | None`,
// `Callable[[int], str]` -- so diagnostics read like ones the user has seen
// before. This is deliberately NOT a claim of message parity with mypy:
// matching mypy's wording verbatim would pin a dependency's phrasing, which
// changes between versions.
//
// Total. A hand-built Type carrying no arguments where the kind expects them
// renders as the bare constructor name rather than as garbage.
//
// Unions render in CONSTRUCTION order, not a canonical one, so a diagnostic
// quotes the union the way the user spelled it. That is why Type::union_of
// normalises identity but not order.
std::string type_name(const Type& type);

} // namespace cythonpp::domain::semantic

#endif // CYTHONPP_DOMAIN_SEMANTIC_TYPE_NAME_H
