#ifndef CYTHONPP_DOMAIN_SEMANTIC_TYPE_H
#define CYTHONPP_DOMAIN_SEMANTIC_TYPE_H

#include <string>
#include <utility>
#include <vector>

#include "type_kind.h"

namespace cythonpp::domain::semantic {

// A Python type: a constructor applied to zero or more arguments.
//
// A copyable value rather than an ast::Node-style hierarchy. The AST is a
// hierarchy because the grammar has genuinely different shapes with different
// children; types are the opposite -- every type has this one shape, so a
// switch over `kind` is exhaustive and -Wswitch-checked where a virtual
// method would not be.
//
// Copyable is the point, and it is the sharpest difference from ast::Node,
// whose copy is deleted: a Type is returned by value from the checker, copied
// into a side table, and compared. It owns nothing but values, so the
// shallow-versus-deep question Node deletes copying to surface does not
// arise here.
//
// `args` is a vector of Type, i.e. recursive. C++17 permits std::vector over
// an incomplete type provided it is complete before any member is referenced
// ([vector.overview]/4), which holds here. Verified to compile clean under
// clang++ -std=c++17 -Wall -Wextra. Do NOT "fix" this into a shared_ptr or a
// pointer-to-vector: it is legal as written, and changing it reintroduces
// pointer chasing and breaks structural ==.
struct Type {
    TypeKind kind = TypeKind::Unknown;

    // Only meaningful for TypeKind::Class, where it is the class's name.
    // Empty otherwise. A class name *is* the constructor, so `kind` plus
    // `name` names the constructor for builtin and user-defined types alike;
    // that one field being used by one kind is a wart accepted deliberately,
    // because a variant or a separate ClassType costs more than the wart.
    std::string name;

    // Element types, union members, or -- for Callable -- the parameter types
    // followed by the return type, RETURN LAST. So a zero-parameter callable
    // has args.size() == 1, and `args.back()` is always the return type.
    // Unambiguous, and stated here because the opposite convention is equally
    // arbitrary and a reader must not have to guess.
    std::vector<Type> args;

    static Type unknown() { return Type{}; }
    static Type none() { return of(TypeKind::NoneType); }
    static Type bool_() { return of(TypeKind::Bool); }
    static Type int_() { return of(TypeKind::Int); }
    static Type float_() { return of(TypeKind::Float); }
    static Type complex_() { return of(TypeKind::Complex); }
    static Type str() { return of(TypeKind::Str); }
    static Type bytes() { return of(TypeKind::Bytes); }
    static Type bytearray_() { return of(TypeKind::ByteArray); }
    static Type ellipsis() { return of(TypeKind::Ellipsis); }
    static Type range_() { return of(TypeKind::Range); }
    static Type object() { return of(TypeKind::Object); }

    static Type list_of(Type element) {
        return parametric(TypeKind::List, {std::move(element)});
    }
    static Type set_of(Type element) { return parametric(TypeKind::Set, {std::move(element)}); }
    static Type frozenset_of(Type element) {
        return parametric(TypeKind::FrozenSet, {std::move(element)});
    }
    static Type dict_of(Type key, Type value) {
        return parametric(TypeKind::Dict, {std::move(key), std::move(value)});
    }
    static Type tuple_of(std::vector<Type> elements) {
        return parametric(TypeKind::Tuple, std::move(elements));
    }

    // Parameters then the return type, matching the `args` convention above.
    static Type callable(std::vector<Type> params, Type result) {
        params.push_back(std::move(result));
        return parametric(TypeKind::Callable, std::move(params));
    }

    static Type class_of(std::string name) {
        Type type;
        type.kind = TypeKind::Class;
        type.name = std::move(name);
        return type;
    }

    // Declared here, DEFINED BELOW operator==. The body needs operator==,
    // which cannot exist until Type is complete, and a member body defined
    // inside the class would not find a later-declared one.
    //
    // Normalises identity but not order: nested unions are flattened and
    // duplicates dropped, so `(int | str) | None` is one three-member union
    // and `int | int` is `int`. A single distinct member collapses to that
    // member and an empty list is Unknown, so a Union always has at least two
    // distinct members -- an invariant every consumer may rely on.
    //
    // Order is deliberately NOT normalised: canonicalising would mean
    // inventing a total order over types, which produces user-visible
    // orderings nobody wrote. is_subtype is order-insensitive instead.
    static Type union_of(std::vector<Type> members);

private:
    static Type of(TypeKind kind) {
        Type type;
        type.kind = kind;
        return type;
    }
    static Type parametric(TypeKind kind, std::vector<Type> args) {
        Type type;
        type.kind = kind;
        type.args = std::move(args);
        return type;
    }
};

// Exact structural equality, and deliberately ORDER-SENSITIVE for unions:
// `int | str` and `str | int` are not ==. That is a trap unless the
// alternative is known, so: is_subtype is order-insensitive for unions, by
// mutual membership. Compatibility -- the thing callers actually ask about --
// does not care about order. This is for tests and for map keys.
inline bool operator==(const Type& left, const Type& right) {
    return left.kind == right.kind && left.name == right.name && left.args == right.args;
}

inline bool operator!=(const Type& left, const Type& right) { return !(left == right); }

inline Type Type::union_of(std::vector<Type> members) {
    std::vector<Type> flat;
    for (Type& member : members) {
        // One level suffices: every Union is built here, and this loop is what
        // maintains the invariant that a Union's args never contain a Union.
        if (member.kind == TypeKind::Union) {
            for (Type& nested : member.args) {
                flat.push_back(std::move(nested));
            }
        } else {
            flat.push_back(std::move(member));
        }
    }

    // Unknown is absorbing here too, and checked BEFORE dedup: a member that
    // failed to resolve means one diagnostic was already reported for it, and
    // a union that carried it forward (`int | Unknown`) would keep comparing
    // as only partially compatible with everything -- is_subtype(int|Unknown,
    // str) is false even though the real annotation is unrecoverable, not
    // "str-shaped" -- so the same root cause would draw a SECOND diagnostic
    // at every later use. Returning Unknown outright is what keeps one root
    // cause to one diagnostic, matching the caller-side guard
    // AnnotationResolver::resolve_union already carries for exactly this
    // reason.
    for (const Type& member : flat) {
        if (member.kind == TypeKind::Unknown) {
            return unknown();
        }
    }

    std::vector<Type> distinct;
    for (Type& candidate : flat) {
        bool seen = false;
        for (const Type& kept : distinct) {
            if (kept == candidate) {
                seen = true;
                break;
            }
        }
        if (!seen) {
            distinct.push_back(std::move(candidate));
        }
    }

    if (distinct.empty()) {
        return unknown();
    }
    if (distinct.size() == 1) {
        return std::move(distinct.front());
    }
    return parametric(TypeKind::Union, std::move(distinct));
}

} // namespace cythonpp::domain::semantic

#endif // CYTHONPP_DOMAIN_SEMANTIC_TYPE_H
