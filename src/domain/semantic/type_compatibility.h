#ifndef CYTHONPP_DOMAIN_SEMANTIC_TYPE_COMPATIBILITY_H
#define CYTHONPP_DOMAIN_SEMANTIC_TYPE_COMPATIBILITY_H

#include <optional>
#include <string>

#include "class_lookup.h"
#include "type.h"
#include "type_kind.h"

namespace cythonpp::domain::semantic {

// Builds the Type a builtin base-name KIND denotes. Shared by two callers
// that both need to turn a bare builtin spelling back into a real Type: the
// class table's own seeding (a seeded base like `bool`'s `int` is a bare
// NAME, exactly like a user `class Sub(int):`) and the ancestor-chain walk
// below, which needs the same conversion for a base recorded this way. One
// function keeps the two from drifting into different answers for the same
// kind.
//
// Exhaustive switch, no default, per project rule: adding a TypeKind forces a
// decision here rather than silently falling through.
//
// The parametric kinds (List, Dict, Set, FrozenSet, Tuple) cannot carry
// arguments as a bare base name -- `class Sub(list): ...` has no syntax for
// list's element type -- so they are modelled as an argument-less container
// (empty `args`). That is a RECORDED imprecision, not a silent one: the
// invariant-container arm in is_subtype requires equal-length args, so a
// class modelled this way is never equal to, say, `list[int]` -- an honest
// consequence of the bare spelling carrying no element type.
//
// Union, Callable and Class can never be a bare base-name spelling --
// builtin_type_kind() never returns them -- so their cases exist only to keep
// this switch exhaustive.
Type builtin_base_type(TypeKind kind);

// The builtin Type a user class's base chain reaches -- Int for
// `class Sub(int)`, Str for `class Name(str)` -- or nullopt when the chain
// reaches no builtin at all. Depth-first left to right, first hit wins,
// through the same cycle-guarded ancestor walk is_subtype and join use, so
// there is no second walk to keep in step with that one.
//
// `object` is excluded, exactly as ClassTable::inherits_builtin excludes it
// and for the same reason: every class conceptually derives from it, so
// counting it would make every class "reach a builtin" and answer Object for
// all of them.
//
// Public because operator_rules' element_type needs it: a class inheriting a
// builtin container is iterable precisely because of what it inherits, and
// deciding that from the Class name alone is impossible. It answers a
// question about a class NAME -- the signature is unchanged -- but the chain
// it walks is now Type-based, so the Type it returns may be PARAMETRIC.
//
// A PARAMETRIC builtin base (`class IntList(list[int])`) is still not
// recorded at all, today: TypeChecker::base_types only records a base via a
// dynamic_cast to ast::Name, so a Subscript base like `list[int]` is dropped
// entirely with no trace, leaving IntList with an EMPTY base list. This
// function therefore still returns nullopt for it, the same answer as for a
// class with no builtin base at all -- not an argument-less `List`. (The
// bare-name spelling, `class L(list)`, IS recorded, but mypy --strict itself
// rejects it: "Missing type parameters for generic type \"list\"".) Callers
// must treat that nullopt as "unknown element type", never as an error.
std::optional<Type> builtin_base_of_class(const ClassLookup& classes, const std::string& name);

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
// differently depending on which side is passed as `left`. For the
// equivalence arm specifically, both sides denote the SAME type, so there is
// no reason to prefer one spelling over the other based on argument
// position: the result is a canonicalised (recursively, into every nested
// Class name in `args`, not just a top-level one), deterministic choice
// between them rather than literally `left`.
//
// join is CROSS THE ARMS ABOVE commutative, but NOT for the nearest-common-
// base search below, where mypy itself is left-biased by design under
// multiple inheritance -- this is not a defect to "fix" into symmetry.
// Verified against mypy 1.18.1: with `class P`, `class Q`, `class X(P, Q)`
// and `class Y(Q, P)`, `join(X, Y)` is `P` while `join(Y, X)` is `Q` -- the
// LEFT operand's FIRST base wins, and the two orders genuinely disagree.
// (Contrast the numeric tower, where join stays commutative: `class S(int)`
// and `class T(float)` join to `float` in both directions, because
// is_subtype's numeric-tower rule -- unlike a base-chain walk -- does not
// care which side started the search.)
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
