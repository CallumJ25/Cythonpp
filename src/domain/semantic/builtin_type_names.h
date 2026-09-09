#ifndef CYTHONPP_DOMAIN_SEMANTIC_BUILTIN_TYPE_NAMES_H
#define CYTHONPP_DOMAIN_SEMANTIC_BUILTIN_TYPE_NAMES_H

#include <optional>
#include <string>

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

} // namespace cythonpp::domain::semantic

#endif // CYTHONPP_DOMAIN_SEMANTIC_BUILTIN_TYPE_NAMES_H
