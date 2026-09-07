#include "type_compatibility.h"

#include <cstddef>

namespace cythonpp::domain::semantic {
namespace {

bool is_invariant_container(TypeKind kind) {
    return kind == TypeKind::List || kind == TypeKind::Dict || kind == TypeKind::Set ||
           kind == TypeKind::FrozenSet;
}

} // namespace

int numeric_rank(TypeKind kind) {
    switch (kind) {
    case TypeKind::Bool:
        return 1;
    case TypeKind::Int:
        return 2;
    case TypeKind::Float:
        return 3;
    case TypeKind::Complex:
        return 4;
    case TypeKind::Unknown:
    case TypeKind::NoneType:
    case TypeKind::Str:
    case TypeKind::Bytes:
    case TypeKind::ByteArray:
    case TypeKind::Ellipsis:
    case TypeKind::List:
    case TypeKind::Dict:
    case TypeKind::Set:
    case TypeKind::FrozenSet:
    case TypeKind::Tuple:
    case TypeKind::Range:
    case TypeKind::Union:
    case TypeKind::Callable:
    case TypeKind::Class:
    case TypeKind::Object:
        return 0;
    }
    // Unreachable: exhaustive above, with no default, so adding a kind warns
    // here rather than silently ranking it non-numeric.
    return 0;
}

bool is_subtype(const Type& source, const Type& target, const ClassLookup* classes) {
    // Absorbing. The error that produced Unknown was already reported, and
    // comparing true is what keeps one root cause to one diagnostic instead
    // of a second report at every use.
    if (source.kind == TypeKind::Unknown || target.kind == TypeKind::Unknown) {
        return true;
    }
    if (source == target) {
        return true;
    }
    if (target.kind == TypeKind::Object) {
        return true;
    }

    // Task 4 inserts the Union, Callable and Class rules here, before the
    // numeric tower -- a Union is not numeric, so it would otherwise fall
    // through to the kind-mismatch return below.

    const int source_rank = numeric_rank(source.kind);
    const int target_rank = numeric_rank(target.kind);
    if (source_rank != 0 && target_rank != 0) {
        return source_rank <= target_rank;
    }

    if (source.kind != target.kind) {
        return false;
    }
    if (is_invariant_container(source.kind)) {
        // Invariant, so identity above was the only way these could succeed;
        // reaching here means the arguments differ. list[float] = list[int]
        // would let a float be appended through the alias, which is why mypy
        // says outright that list is invariant.
        return false;
    }
    if (source.kind == TypeKind::Tuple) {
        // Covariant, elementwise, because a tuple is immutable. Arity must
        // match: a two-tuple is not a one-tuple.
        if (source.args.size() != target.args.size()) {
            return false;
        }
        for (std::size_t index = 0; index < source.args.size(); ++index) {
            if (!is_subtype(source.args[index], target.args[index], classes)) {
                return false;
            }
        }
        return true;
    }
    return false;
}

} // namespace cythonpp::domain::semantic
