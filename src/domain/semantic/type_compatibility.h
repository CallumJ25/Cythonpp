#ifndef CYTHONPP_DOMAIN_SEMANTIC_TYPE_COMPATIBILITY_H
#define CYTHONPP_DOMAIN_SEMANTIC_TYPE_COMPATIBILITY_H

#include "class_lookup.h"
#include "type.h"
#include "type_kind.h"

namespace cythonpp::domain::semantic {

// Position in Python's numeric tower -- Bool 1, Int 2, Float 3, Complex 4 --
// or 0 for a kind outside it.
//
// Public because is_subtype needs it for the tower's assignability and
// operator_rules needs it for arithmetic's result type, and two copies of the
// tower would drift into two different answers.
int numeric_rank(TypeKind kind);

// Whether a value of type `source` may be used where `target` is expected.
//
// Named is_subtype rather than is_assignable on purpose:
// domain/parser/assignability.h already owns is_assignable(const ast::Expr&),
// meaning "is this expression a legal assignment target". Two functions of
// that name in adjacent namespaces answering unrelated questions is a defect
// waiting for a tired reader.
//
// Order-INSENSITIVE for unions, by mutual membership, unlike Type's
// operator==, which is exact. Compatibility does not care how a union was
// spelled.
//
// `classes` is needed only to walk a class's base chain, so it may be null;
// two Class types with different names are then simply unrelated. A pointer
// rather than a reference because most callers have no class table and
// requiring one would mean inventing an empty implementation at every site.
bool is_subtype(const Type& source, const Type& target, const ClassLookup* classes = nullptr);

} // namespace cythonpp::domain::semantic

#endif // CYTHONPP_DOMAIN_SEMANTIC_TYPE_COMPATIBILITY_H
