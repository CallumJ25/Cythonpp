#ifndef CYTHONPP_DOMAIN_SEMANTIC_TYPE_KIND_H
#define CYTHONPP_DOMAIN_SEMANTIC_TYPE_KIND_H

namespace cythonpp::domain::semantic {

// What a Type is a constructor for.
//
// Unknown is first so a value-initialized Type is the absorbing element
// rather than a plausible-looking NoneType. Object is last because it is the
// top of the lattice, not a builtin alongside the others. Both positions are
// load-bearing, not cosmetic.
//
// An ordinary scoped enum with no underlying type, unlike token_category and
// token_type: nothing here is bit-packed or persisted, so the values carry no
// meaning beyond distinctness.
//
// NoneType rather than None because None is an object-like macro in X11's
// X.h, and macro substitution happens before scoping -- the same hazard
// diagnostic.h records for Severity::ERROR. It is also Python's own name for
// the type of None.
enum class TypeKind {
    Unknown,

    NoneType,
    Bool,
    Int,
    Float,
    Complex,
    Str,
    Bytes,
    ByteArray,
    Ellipsis,

    List,
    Dict,
    Set,
    FrozenSet,
    Tuple,
    Range,

    Union,
    Callable,
    Class,

    Object,
};

} // namespace cythonpp::domain::semantic

#endif // CYTHONPP_DOMAIN_SEMANTIC_TYPE_KIND_H
