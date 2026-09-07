#include "type_compatibility.h"

#include <cstddef>
#include <string>
#include <vector>

namespace cythonpp::domain::semantic {
namespace {

bool is_invariant_container(TypeKind kind) {
    return kind == TypeKind::List || kind == TypeKind::Dict || kind == TypeKind::Set ||
           kind == TypeKind::FrozenSet;
}

// Whether `derived` reaches `base` through its base chain.
//
// Iterative with an explicit worklist and a visited list rather than
// recursive: a malformed class table can contain a cycle -- `class A(B)` with
// `class B(A)` is rejected by Python, but nothing here guarantees the table
// it is handed is well-formed -- and a recursive walk would not return.
bool inherits_from(const ClassLookup& classes, const std::string& derived,
                   const std::string& base) {
    std::vector<std::string> pending = classes.bases_of(derived);
    std::vector<std::string> seen;

    while (!pending.empty()) {
        const std::string current = pending.back();
        pending.pop_back();
        if (current == base) {
            return true;
        }

        bool already_seen = false;
        for (const std::string& visited : seen) {
            if (visited == current) {
                already_seen = true;
                break;
            }
        }
        if (already_seen) {
            continue;
        }
        seen.push_back(current);

        const std::vector<std::string> bases = classes.bases_of(current);
        for (const std::string& next : bases) {
            pending.push_back(next);
        }
    }
    return false;
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

    // Every member must be assignable, so `int | str` goes only where both
    // int and str go. Placed before the target-union rule so a union-to-union
    // check resolves by recursion: each member is then checked against the
    // target union by the arm below.
    if (source.kind == TypeKind::Union) {
        for (const Type& member : source.args) {
            if (!is_subtype(member, target, classes)) {
                return false;
            }
        }
        return true;
    }
    if (target.kind == TypeKind::Union) {
        for (const Type& member : target.args) {
            if (is_subtype(source, member, classes)) {
                return true;
            }
        }
        return false;
    }
    if (source.kind == TypeKind::Callable && target.kind == TypeKind::Callable) {
        // args is parameters followed by the return type, so equal sizes mean
        // equal arity. Empty args is unreachable through Type::callable but
        // is rejected rather than indexed into.
        if (source.args.empty() || target.args.empty() ||
            source.args.size() != target.args.size()) {
            return false;
        }
        const std::size_t parameters = source.args.size() - 1;
        for (std::size_t index = 0; index < parameters; ++index) {
            // Contravariant: the target's parameter must be acceptable to the
            // source, not the other way round. A function taking float can
            // stand in where one taking int is wanted; one taking bool cannot.
            if (!is_subtype(target.args[index], source.args[index], classes)) {
                return false;
            }
        }
        // Covariant in the return type.
        return is_subtype(source.args.back(), target.args.back(), classes);
    }
    if (source.kind == TypeKind::Class && target.kind == TypeKind::Class) {
        return classes != nullptr && inherits_from(*classes, source.name, target.name);
    }

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
