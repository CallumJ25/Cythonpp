#ifndef CYTHONPP_DOMAIN_SEMANTIC_BUILTIN_TYPE_NAMES_H
#define CYTHONPP_DOMAIN_SEMANTIC_BUILTIN_TYPE_NAMES_H

#include <optional>
#include <string>

#include "type.h"
#include "type_kind.h"

namespace cythonpp::domain::semantic {

// `tuple` takes any number of arguments.
constexpr int kVariadicArity = -1;

// Spelling -> TypeKind, for the builtin names this model represents as KINDS
// rather than as seeded Class entries.
//
// Extracted from annotation_resolver.cpp's anonymous namespace, where it was
// invisible to is_subtype -- the mechanical reason
// is_subtype(Class("Sub"), Int) was false for `class Sub(int)`. The Class arm
// needs to ask whether a base NAME denotes a builtin kind, and could not.
//
// std::nullopt for anything else, including builtin names that are functions
// (`print`) and builtin CLASSES outside the model's kind set (`Exception`,
// `slice`, `type`). Those are seeded into the class table instead; there is no
// TypeKind::Exception and there should not be.
//
// Consulted BEFORE ClassLookup::is_class, which is why seeding `int` into the
// class table alongside the exception hierarchy is harmless.
std::optional<TypeKind> builtin_type_kind(const std::string& name);

// Required type-argument count: 0 for the scalars, 1 for list/set/frozenset,
// 2 for dict, kVariadicArity for tuple, 0 for an unknown name.
int builtin_type_arity(const std::string& name);

// TypeKind -> the builtin SPELLING this model represents it by, the exact
// reverse of builtin_type_kind above and derived from the same table, so the
// two cannot drift into disagreeing. std::nullopt for a kind that is not a
// builtin spelling at all (Unknown, Union, Callable, Class, Ellipsis).
//
// Exists because a class's bases are stored as Types, and the base-chain walk
// still needs a NAME to look an Entry up by: a base recorded as
// Type::list_of(int) has to become "list" to reach `list`'s own entry in the
// seeded builtin class table. Deriving the name rather than storing it
// alongside keeps one source of truth for what a base IS.
std::optional<std::string> builtin_type_spelling(TypeKind kind);

// Builds the Type a builtin KIND denotes on its own, with no name attached.
// `base_type_for_name` below is built directly on top of this and is what
// `ClassTable`'s constructor seeding and `TypeChecker::base_types` actually
// call; this function's own remaining direct callers are
// `type_compatibility.cpp`'s `builtin_base_of_class`, which needs the
// conversion for the chain root's own builtin identity once a NAME has
// already been classified into a TypeKind, and tests that need to construct
// a bare container Type literal (e.g. `builtin_base_type(TypeKind::List)`)
// with no source-level name to classify at all.
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

// `name` classified into a Type the same way everywhere it is needed: a name
// this model represents as a builtin KIND becomes that kind's bare Type
// (via builtin_base_type); anything else is an ordinary Class. One function
// rather than the three separate copies this rule used to have --
// `ClassTable`'s constructor seeding, `TypeChecker::base_types`, and even
// `FakeClassLookup::to_typed` in tests/domain/semantic/fake_class_lookup.h --
// each writing out the identical two-line ternary. A test fake was one of
// the three copies, which is precisely the danger: a future change to this
// rule could land in the two production copies while the fake silently kept
// the old belief, and the tests built on it would keep passing for the wrong
// reason.
Type base_type_for_name(const std::string& name);

} // namespace cythonpp::domain::semantic

#endif // CYTHONPP_DOMAIN_SEMANTIC_BUILTIN_TYPE_NAMES_H
