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

// Whether two types are the SAME type, as opposed to one being assignable to
// the other. Mutual assignability rather than operator==, because == is exact
// and order-sensitive for unions while `int | str` and `str | int` are the
// same type. Use this -- never == -- wherever a rule needs type identity:
// invariant containers, list concatenation, set operations, ordered
// comparisons.
bool is_equivalent(const Type& left, const Type& right, const ClassLookup* classes = nullptr);

// The element type mypy infers for an un-annotated container display holding
// both `left` and `right` -- `[1, "s"]` is `list[object]`. Task 13 folds this
// pairwise over every element.
//
// Deliberately NOT union_of. Verified against mypy 1.18.1: mypy computes a
// least upper bound over the nominal hierarchy, not a union -- `[1, "s"]` is
// `list[object]`, not `list[int | str]`. The one exception is `None`, which
// mypy really does union: `[1, None]` is `list[int | None]`.
//
// Uses is_equivalent, never operator==, to recognise "the same type": `[x, y]`
// where `x: list[int | str]` and `y: list[str | int]` must join to that list
// type, not fall through to Object merely because the two unions were spelled
// in a different order. Using == there was Spec 5a's Critical defect.
//
// Equivalent does not mean identical: that same union-order case, and an
// aliased class name (IOError vs. its canonical OSError), both render
// differently depending on which side is passed as `left`. join is
// commutative -- join(a, b) and join(b, a) always agree -- so when the two
// sides are merely equivalent, the result is a canonicalised, deterministic
// choice between them rather than literally `left`.
//
// Deliberately NOT recursive into invariant type arguments. `[[1], ["a"]]` is
// `list[object]`, verified -- NOT `list[list[object]]`. join is applied once,
// at the display's own element level; it does not descend into a `list`,
// `dict`, `set` or `frozenset` argument to join those pairwise in turn. Do not
// "fix" this into a recursive join: mypy never infers the recursive answer.
//
// `classes` is needed only to walk a class's base chain (nearest common base,
// or a class chain reaching a builtin kind), exactly as is_subtype needs it,
// so it may be null; two Class types are then simply unrelated and join to
// Object.
Type join(const Type& left, const Type& right, const ClassLookup* classes = nullptr);

} // namespace cythonpp::domain::semantic

#endif // CYTHONPP_DOMAIN_SEMANTIC_TYPE_COMPATIBILITY_H
