#include "type_compatibility.h"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "builtin_type_names.h"
#include "type_name.h"

namespace cythonpp::domain::semantic {
namespace {

bool is_invariant_container(TypeKind kind) {
    return kind == TypeKind::List || kind == TypeKind::Dict || kind == TypeKind::Set ||
           kind == TypeKind::FrozenSet;
}

// Builds the Type a builtin base-name KIND denotes, for the base-chain walk
// below. Task 6 made builtin_type_kind visible outside
// annotation_resolver.cpp precisely so this becomes possible: a base spelled
// "int" -- `class Sub(int): ...` -- can now be turned back into a real Type
// and handed to the ordinary is_subtype rules, rather than being unreachable.
//
// Exhaustive switch, no default, per project rule: adding a TypeKind forces a
// decision here rather than silently falling through.
//
// The parametric kinds (List, Dict, Set, FrozenSet, Tuple) cannot carry
// arguments as a bare base name -- `class Sub(list): ...` has no syntax for
// list's element type -- so they are modelled as an argument-less container
// (empty `args`). That is a RECORDED imprecision, not a silent one: the
// invariant-container arm below requires equal-length args, so a class
// modelled this way is never equal to, say, `list[int]` -- an honest
// consequence of the bare spelling carrying no element type.
//
// Union, Callable and Class can never be a bare base-name spelling --
// builtin_type_kind() never returns them -- so their cases exist only to keep
// this switch exhaustive.
Type builtin_base_type(TypeKind kind) {
    switch (kind) {
    case TypeKind::Unknown:
        return Type::unknown();
    case TypeKind::NoneType:
        return Type::none();
    case TypeKind::Bool:
        return Type::bool_();
    case TypeKind::Int:
        return Type::int_();
    case TypeKind::Float:
        return Type::float_();
    case TypeKind::Complex:
        return Type::complex_();
    case TypeKind::Str:
        return Type::str();
    case TypeKind::Bytes:
        return Type::bytes();
    case TypeKind::ByteArray:
        return Type::bytearray_();
    case TypeKind::Ellipsis:
        return Type::ellipsis();
    case TypeKind::Range:
        return Type::range_();
    case TypeKind::Object:
        return Type::object();
    case TypeKind::List:
    case TypeKind::Dict:
    case TypeKind::Set:
    case TypeKind::FrozenSet:
    case TypeKind::Tuple:
    case TypeKind::Union:
    case TypeKind::Callable:
    case TypeKind::Class: {
        Type type;
        type.kind = kind;
        return type;
    }
    }
    // Unreachable: exhaustive above, with no default, so adding a kind warns
    // here rather than silently mis-modelling it.
    return Type::unknown();
}

// Whether `derived`'s base chain reaches something assignable to `target`.
// A visited base NAME may denote either another class (walked further) or a
// builtin KIND -- recursing through the ordinary is_subtype rules once one is
// found means the numeric tower (int -> float -> complex) and the Object top
// arm apply without being restated in this walk.
//
// `derived` is expected already canonicalised by the caller (is_subtype);
// every name visited DURING the walk is canonicalised here, once, at the
// point it is read off `pending` -- the single place that feeds both the
// target-name comparison and the `seen` guard, so canonicalisation can never
// let the same class be visited twice under two different spellings (which
// would also risk the cycle guard missing a cycle spelled inconsistently).
//
// Only one ClassLookup parameter: `classes` is also what the recursive
// is_subtype call below needs, and every caller already has exactly one
// lookup in hand -- a second parameter for "the same object, as a pointer"
// answered no question a caller could ever answer differently.
//
// Iterative with an explicit worklist and a visited list rather than
// recursive: a malformed class table can contain a cycle -- `class A(B)` with
// `class B(A)` is rejected by Python, but nothing here guarantees the table
// it is handed is well-formed -- and a recursive walk would not return.
bool class_reaches(const ClassLookup& classes, const std::string& derived, const Type& target) {
    const std::string canonical_target =
        target.kind == TypeKind::Class ? classes.canonical_name(target.name) : std::string();

    std::vector<std::string> pending = classes.bases_of(derived);
    std::vector<std::string> seen;

    while (!pending.empty()) {
        const std::string current = classes.canonical_name(pending.back());
        pending.pop_back();

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

        if (target.kind == TypeKind::Class && current == canonical_target) {
            return true;
        }
        if (const std::optional<TypeKind> kind = builtin_type_kind(current)) {
            if (is_subtype(builtin_base_type(*kind), target, &classes)) {
                return true;
            }
        }

        const std::vector<std::string> bases = classes.bases_of(current);
        for (const std::string& next : bases) {
            pending.push_back(next);
        }
    }
    return false;
}

// Every class canonically reachable from `start`, closest first, with `start`
// itself (canonicalised) included as the first element -- so a common-base
// search comparing two chains can find that one class is simply a base of the
// other, not only a shared ancestor further up.
//
// Same iterative worklist + visited-list shape as class_reaches, and for the
// same reason: `class A(B)` / `class B(A)` is constructible and nothing here
// may assume the table it is handed is well-formed, so a recursive walk would
// not return.
std::vector<std::string> class_ancestor_chain(const ClassLookup& classes, const std::string& start) {
    std::vector<std::string> chain = {classes.canonical_name(start)};
    std::vector<std::string> pending = classes.bases_of(chain.front());

    while (!pending.empty()) {
        const std::string current = classes.canonical_name(pending.back());
        pending.pop_back();

        bool already_seen = false;
        for (const std::string& visited : chain) {
            if (visited == current) {
                already_seen = true;
                break;
            }
        }
        if (already_seen) {
            continue;
        }
        chain.push_back(current);

        const std::vector<std::string> bases = classes.bases_of(current);
        for (const std::string& next : bases) {
            pending.push_back(next);
        }
    }
    return chain;
}

// The nearest common base of two Class types, for join's both-Class arm.
// Walks `left`'s ancestor chain closest-first (self included) and returns the
// first ancestor that `right` is also a subtype of -- for single inheritance
// this is exactly mypy's nominal least-upper-bound. A chain entry may itself
// be a builtin base name (`class Sub(int)`), so each entry is turned back
// into a real Type via builtin_type_kind before the is_subtype check, exactly
// as class_reaches does.
//
// Nothing seeds Object into a user class's bases_of() chain, so two genuinely
// unrelated classes fall off the end of the loop; the caller supplies Object.
Type nearest_common_base(const ClassLookup& classes, const Type& left, const Type& right) {
    for (const std::string& ancestor : class_ancestor_chain(classes, left.name)) {
        Type candidate;
        if (const std::optional<TypeKind> kind = builtin_type_kind(ancestor)) {
            candidate = builtin_base_type(*kind);
        } else {
            candidate = Type::class_of(ancestor);
        }
        if (is_subtype(right, candidate, &classes)) {
            return candidate;
        }
    }
    return Type::object();
}

// A copy of `type` with its Class name (if any) resolved through `classes`.
// Class::name is the one field two otherwise-equivalent Types can legitimately
// differ on for a reason the caller does not control:
// EnvironmentError/IOError/WindowsError are one class under three spellings.
// This is what lets join's equivalence tie-break (below) pick the ONE
// spelling ClassLookup considers real, rather than whichever alias happened
// to be passed as the first argument.
Type canonicalised(Type type, const ClassLookup* classes) {
    if (classes != nullptr && type.kind == TypeKind::Class) {
        type.name = classes->canonical_name(type.name);
    }
    return type;
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
    if (source.kind == TypeKind::Class) {
        // Broader than "target is also Class": a user class's base chain can
        // reach a builtin KIND, not just another class -- `class Sub(int)`
        // makes `x: int = Sub()` mypy --strict clean, so target may be Int,
        // Float or Complex here too. class_reaches walks both kinds of base
        // uniformly. Without a lookup there is no chain to walk, so two
        // Class types (or a Class and a builtin kind) are simply unrelated,
        // matching the pre-Task-9 behaviour exactly.
        if (classes == nullptr) {
            return false;
        }
        // Canonicalise the source before comparing or walking: an
        // EnvironmentError/IOError/WindowsError spelling and its OSError
        // canonical are the SAME class, not two related-but-distinct ones,
        // so a source and target that denote one class under two different
        // spellings must compare equal by IDENTITY here, not merely via a
        // base-chain walk that would never find one as a base of the other.
        // This is the fix for the regression where AnnotationResolver and
        // ClassTable could each hand back a Type::class_of(...) spelled
        // differently for the one class.
        const std::string canonical_source = classes->canonical_name(source.name);
        if (target.kind == TypeKind::Class &&
            canonical_source == classes->canonical_name(target.name)) {
            return true;
        }
        return class_reaches(*classes, canonical_source, target);
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
        // Invariant, so a MUTATION through the alias must stay sound --
        // list[float] = list[int] would let a float be appended to what is
        // really a list[int], which is why mypy says outright that list is
        // invariant. But invariant means the two ELEMENT types must be the
        // same type, not that they must be spelled identically: operator==
        // already failed above (or this arm would be unreached), yet
        // list[int | str] and list[str | int] are the same type with a
        // differently-ordered union spelling. Elementwise is_equivalent,
        // never ==, is what tells those apart from a genuine list[int] vs
        // list[float] mismatch.
        if (source.args.size() != target.args.size()) {
            return false;
        }
        for (std::size_t index = 0; index < source.args.size(); ++index) {
            if (!is_equivalent(source.args[index], target.args[index], classes)) {
                return false;
            }
        }
        return true;
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

bool is_equivalent(const Type& left, const Type& right, const ClassLookup* classes) {
    return is_subtype(left, right, classes) && is_subtype(right, left, classes);
}

Type join(const Type& left, const Type& right, const ClassLookup* classes) {
    // Absorbing bottom, checked first: one root cause draws one diagnostic,
    // not a fresh "unrelated types" complaint at every later use of an
    // un-annotated display that already failed to type one element.
    if (left.kind == TypeKind::Unknown || right.kind == TypeKind::Unknown) {
        return Type::unknown();
    }
    // Must run before the None arm below: two Nones are equivalent, and this
    // is what makes them collapse to None rather than union to `None | None`
    // (which union_of would in fact also collapse, but via a different path
    // than intended -- the equivalence check is the one actually specified).
    // Uses is_equivalent, never ==, so list[int | str] and list[str | int]
    // are recognised as the one type despite the differently-ordered union
    // spelling.
    //
    // Equivalent does not mean identical, though: that same union-order case
    // renders differently depending on which side happens to be "left", and
    // an aliased class (IOError vs. its canonical OSError) does too. join
    // must be commutative -- join(A, B) and join(B, A) the SAME answer -- so
    // rather than literally returning `left`, canonicalise each side's Class
    // spelling and then break any remaining tie by rendered name. That makes
    // the result a function of the unordered pair {left, right}, never of
    // which argument position the caller happened to use.
    if (is_equivalent(left, right, classes)) {
        const Type canonical_left = canonicalised(left, classes);
        const Type canonical_right = canonicalised(right, classes);
        return type_name(canonical_left) <= type_name(canonical_right) ? canonical_left
                                                                        : canonical_right;
    }
    // THE exception to "join, don't union": None really does union. Checked
    // before the numeric-tower and Class arms below, neither of which could
    // ever fire for a NoneType operand anyway (NoneType's numeric_rank is 0
    // and its kind is never Class), but stated here as its own arm because
    // that is where the rule table places it.
    if (left.kind == TypeKind::NoneType || right.kind == TypeKind::NoneType) {
        const Type& other = left.kind == TypeKind::NoneType ? right : left;
        return Type::union_of({other, Type::none()});
    }

    // Must run before the Class arm: numeric_rank is 0 for TypeKind::Class,
    // so the two arms cannot both fire for the same pair, but ordering them
    // this way matches the rule table and keeps the numeric tower's own
    // reasoning (a wider rank is always a supertype) from ever being
    // shadowed by a class-chain walk.
    const int left_rank = numeric_rank(left.kind);
    const int right_rank = numeric_rank(right.kind);
    if (left_rank != 0 && right_rank != 0) {
        return left_rank >= right_rank ? left : right;
    }

    if (classes != nullptr) {
        if (left.kind == TypeKind::Class && right.kind == TypeKind::Class) {
            return nearest_common_base(*classes, left, right);
        }
        // One side a class whose base chain reaches the other side's builtin
        // kind -- `class Sub(int)` joins with `1` to `int`, and through the
        // tower, to `float` or `complex` too, via class_reaches recursing
        // into is_subtype's numeric-tower arm.
        if (left.kind == TypeKind::Class &&
            class_reaches(*classes, classes->canonical_name(left.name), right)) {
            return right;
        }
        if (right.kind == TypeKind::Class &&
            class_reaches(*classes, classes->canonical_name(right.name), left)) {
            return left;
        }
    }

    // Anything else -- unrelated scalars, unrelated classes, two containers
    // whose element types differ (join does not recurse into them) -- joins
    // to Object, mypy's least upper bound when nothing narrower is common.
    return Type::object();
}

} // namespace cythonpp::domain::semantic
