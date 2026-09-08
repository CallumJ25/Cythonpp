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

// Strips TypeChecker's own
// synthetic class-isolation prefix -- "<tag>#<line>#" (see
// type_checker.cpp's declare_isolated_class) -- from a class's qualified
// name, leaving the ORIGINAL, user-written (possibly dotted) name. A losing
// top-level class redefinition, and EVERY
// function-local class, is declared under such a synthetic name so its
// ClassTable entry can never collide with a real one; '<', '>' and '#' are
// used specifically because no Python identifier can contain them. That
// uniqueness guarantee is an internal ClassTable-key concern only -- left
// unstripped, it leaked verbatim into a user-facing diagnostic (an
// attr-defined message naming an isolated class, say), quoting a "type"
// that appears nowhere in the user's source. type_name's own Class case
// routes through this; a raw `Type::name`/`ClassTable` name used directly
// in a hand-built message (expression_typer.cpp's attr-defined message,
// expression_typer_calls.cpp's method-call label) must route through this
// too, rather than through type_name itself, since neither builds a Type to
// call type_name on. A name with no such prefix is returned unchanged, so
// this is safe to call on every class name unconditionally.
std::string strip_synthetic_class_prefix(const std::string& name);

} // namespace cythonpp::domain::semantic

#endif // CYTHONPP_DOMAIN_SEMANTIC_TYPE_NAME_H
