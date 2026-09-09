#include "type_compatibility.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "builtin_type_names.h"

namespace cythonpp::domain::semantic {
namespace {

bool is_invariant_container(TypeKind kind) {
    return kind == TypeKind::List || kind == TypeKind::Dict || kind == TypeKind::Set ||
           kind == TypeKind::FrozenSet;
}

// The ClassLookup key a chain step is looked up under. The exact analogue of
// ClassTable::base_key, kept separate only because that one is a private of
// the concrete table and this one works through the abstract lookup; keep the
// two in step.
std::optional<std::string> chain_key(const ClassLookup& classes, const Type& step) {
    if (step.kind == TypeKind::Class) {
        return classes.canonical_name(step.name);
    }
    return builtin_type_spelling(step.kind);
}

// Every class canonically reachable from `start`, DEPTH-FIRST LEFT TO RIGHT,
// with `start` itself (canonicalised) included as the first element -- so a
// common-base search comparing two chains can find that one class is simply
// a base of the other, not only a shared ancestor further up. Matches
// class_table.h's own stated convention ("Depth-first, left to right through
// the base chain") and its recursive walk_chain exactly: `class X(P, Q)`
// with `Q(R)` yields `[X, P, Q, R]`, visiting P (and its whole subtree, if
// it had one) before Q, never the reverse. That ordering is load-bearing for
// join's nearest-common-base search below, which is deliberately left-biased
// under multiple inheritance to match mypy.
//
// Entries are TYPES, not names, so a PARAMETRIC base survives the walk:
// `class IntList(list[int])` yields `[Class("IntList"), list[int], object]`
// and the element type is still there for is_subtype and for the element/
// subscript rules to use. That is the whole point of the change -- with names
// only, `list[int]` collapsed to "list" and every element type was lost.
//
// A step's ClassTable key is its own name for a Class, or the builtin
// spelling for a builtin kind (so `list[int]` still reaches `list`'s seeded
// entry and its `object` base). A step that denotes no key ends that branch.
//
// Iterative with an explicit worklist and `chain` doubling as the visited
// list, for the same reason as before: a malformed class table can contain a
// cycle. Bases are pushed in REVERSE so the leftmost is popped first --
// the standard iterative-preorder trick -- and that left-to-right order is
// load-bearing for nearest_common_base's deliberately left-biased search.
//
// Membership is by KEY, not by whole Type: two different parameterisations of
// one base cannot both be walked, and comparing whole Types would let
// `list[int]` and `list[str]` both enter the chain and defeat the cycle
// guard.
//
// THE ONE guarded walk: class_reaches (below) is expressed in terms of this
// function rather than repeating the cycle guard a second time.
std::vector<Type> class_ancestor_chain(const ClassLookup& classes, const std::string& start) {
    const std::string root = classes.canonical_name(start);
    std::vector<Type> chain = {Type::class_of(root)};
    std::vector<std::string> keys = {root};

    std::vector<Type> pending = classes.bases_of(root);
    std::reverse(pending.begin(), pending.end());

    while (!pending.empty()) {
        Type current = std::move(pending.back());
        pending.pop_back();
        if (current.kind == TypeKind::Class) {
            current.name = classes.canonical_name(current.name);
        }

        const std::optional<std::string> key = chain_key(classes, current);
        if (!key.has_value()) {
            continue;
        }
        if (std::find(keys.begin(), keys.end(), *key) != keys.end()) {
            continue;
        }
        keys.push_back(*key);
        chain.push_back(current);

        std::vector<Type> bases = classes.bases_of(*key);
        std::reverse(bases.begin(), bases.end());
        for (Type& next : bases) {
            pending.push_back(std::move(next));
        }
    }
    return chain;
}

// Whether `derived`'s base chain reaches something assignable to `target`.
// A visited base NAME may denote either another class (walked further) or a
// builtin KIND -- recursing through the ordinary is_subtype rules once one is
// found means the numeric tower (int -> float -> complex) and the Object top
// arm apply without being restated in this walk.
//
// Expressed in terms of class_ancestor_chain rather than its own worklist:
// class_table.h:126-127 states the project rule that the cycle guard exists
// in exactly one place, and this function's walk was previously a
// line-for-line copy of that one. `derived` itself (chain[0]) is skipped --
// class_reaches only ever asked about derived's PROPER ancestors, matching
// the old code's own starting worklist of `bases_of(derived)` rather than
// `{derived}`.
//
// Traversal order does not matter for correctness here (unlike in
// nearest_common_base): this function stops at the first match found by
// EITHER search, not the first found in chain order, so any order that
// visits every ancestor exactly once answers the same true/false.
//
// Only one ClassLookup parameter: `classes` is also what the recursive
// is_subtype call below needs, and every caller already has exactly one
// lookup in hand -- a second parameter for "the same object, as a pointer"
// answered no question a caller could ever answer differently.
bool class_reaches(const ClassLookup& classes, const std::string& derived, const Type& target) {
    const std::string canonical_target =
        target.kind == TypeKind::Class ? classes.canonical_name(target.name) : std::string();

    const std::vector<Type> chain = class_ancestor_chain(classes, derived);
    for (std::size_t index = 1; index < chain.size(); ++index) {
        const Type& current = chain[index];

        if (current.kind == TypeKind::Class) {
            // Compared by NAME rather than through is_subtype: recursing
            // there for a Class step would come straight back into this
            // function for the same chain and never terminate.
            if (target.kind == TypeKind::Class && current.name == canonical_target) {
                return true;
            }
            continue;
        }
        // A builtin step, now carrying its type ARGUMENTS: `class
        // IntList(list[int])` reaches list[int], so `x: list[int] =
        // IntList()` is clean and the reverse direction correctly is not
        // (is_subtype is asymmetric, and the invariant-container arm below
        // handles it). Routing through the ordinary rules is also what makes
        // the numeric tower apply for `class Sub(int)` without restating it.
        if (is_subtype(current, target, &classes)) {
            return true;
        }
    }
    return false;
}

// The nearest common base of two Class types, for join's both-Class arm.
//
// Walks `left`'s ancestor chain closest-first (self included, depth-first
// left to right per class_ancestor_chain) and returns the first ancestor
// that `right` is also a subtype of. For single inheritance this is exactly
// mypy's nominal least-upper-bound; for multiple inheritance it is
// DELIBERATELY left-biased, matching mypy itself -- verified against mypy
// 1.18.1: `class P`, `class Q`, `class X(P, Q)`, `class Y(Q, P)` join
// `join(X, Y)` to `P` and `join(Y, X)` to `Q`, the LEFT operand's first base
// winning each time. Do not "fix" this into a symmetric search; that would
// disagree with mypy.
//
// If left's chain is exhausted with no match, that does NOT mean the two are
// unrelated: is_subtype recognises a supertype that appears in NO bases_of
// chain at all -- the numeric tower. `class S(int)` and `class T(float)`
// share no name in either chain, yet is_subtype(Class(S), float) is true, so
// a left-chain-only search would find `object` for `join(S, T)` while
// finding `float` for `join(T, S)` -- verified WRONG against mypy 1.18.1,
// which says `float` in BOTH directions there (the numeric tower, unlike
// multiple inheritance, is not left-biased). Falling back to a walk of
// right's chain -- testing `left` against each candidate -- catches exactly
// this case. This fallback cannot undo the left-bias above: the left chain
// is always searched to exhaustion FIRST, in its own left-to-right order, so
// any match reachable from `left`'s side -- including the multiple-
// inheritance case above, where each of X and Y sits directly in the
// other's base chain -- is found and returned before the right-chain loop
// ever runs. The right-chain walk only ever contributes an answer the left
// chain could not have found by any ordering: a supertype belonging to
// neither operand's nominal bases_of chain at all.
//
// Nothing seeds Object into a user class's bases_of() chain, so two
// genuinely unrelated classes fall off the end of both loops; the caller
// supplies Object.
Type nearest_common_base(const ClassLookup& classes, const Type& left, const Type& right) {
    for (const Type& candidate : class_ancestor_chain(classes, left.name)) {
        if (is_subtype(right, candidate, &classes)) {
            return candidate;
        }
    }
    for (const Type& candidate : class_ancestor_chain(classes, right.name)) {
        if (is_subtype(left, candidate, &classes)) {
            return candidate;
        }
    }
    return Type::object();
}

// Structural order over Type -- kind, then name, then args pairwise --
// deliberately NOT type_name. type_name.h states outright that its rendering
// is a diagnostics spelling, not pinned across changes; keying a domain
// decision (which of two equivalent Types join returns) on that presentation
// function would let an unrelated rendering change silently flip the answer.
// A strict weak order, used only to break the equivalence-arm tie
// deterministically -- there is no meaning attached to "less than" beyond
// picking one of two equivalent Types in a way that does not depend on which
// was passed as `left`.
bool type_less(const Type& left, const Type& right) {
    if (left.kind != right.kind) {
        return left.kind < right.kind;
    }
    if (left.name != right.name) {
        return left.name < right.name;
    }
    if (left.args.size() != right.args.size()) {
        return left.args.size() < right.args.size();
    }
    for (std::size_t index = 0; index < left.args.size(); ++index) {
        if (type_less(left.args[index], right.args[index])) {
            return true;
        }
        if (type_less(right.args[index], left.args[index])) {
            return false;
        }
    }
    if (left.defaulted_params != right.defaulted_params) {
        return left.defaulted_params < right.defaulted_params;
    }
    return false;
}

// A copy of `type` with every Class name reachable from it -- its own, and
// any nested inside `args` -- resolved through `classes`. Class::name is the
// one field two otherwise-equivalent Types can legitimately differ on for a
// reason the caller does not control: EnvironmentError/IOError/WindowsError
// are one class under three spellings. This is what lets join's equivalence
// tie-break (below) pick the ONE spelling ClassLookup considers real, rather
// than whichever alias happened to be passed as the first argument --
// recursively, so `join(list[IOError], list[OSError])` returns
// `list[OSError]`, not `list[IOError]` left untouched inside an otherwise-
// canonicalised container.
Type canonicalised(Type type, const ClassLookup* classes) {
    if (classes == nullptr) {
        return type;
    }
    if (type.kind == TypeKind::Class) {
        type.name = classes->canonical_name(type.name);
    }
    for (Type& arg : type.args) {
        arg = canonicalised(std::move(arg), classes);
    }
    return type;
}

} // namespace

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

std::optional<Type> builtin_base_of_class(const ClassLookup& classes, const std::string& name) {
    // The whole chain including index 0, unlike class_reaches, which asks
    // only about PROPER ancestors: a caller passing a name that is itself a
    // builtin spelling should get that builtin back rather than nothing,
    // since "what builtin does this name denote or inherit" is one question.
    //
    // `object` is skipped for the reason inherits_builtin's own comment
    // gives: every class conceptually derives from it, and treating it as
    // "the inherited builtin" would answer this question `true` for every
    // class in the program.
    //
    // The step is returned AS RECORDED, type arguments and all, which is what
    // makes `class IntList(list[int])` subscript and iterate as `int` rather
    // than deferring.
    for (const Type& step : class_ancestor_chain(classes, name)) {
        if (step.kind == TypeKind::Class || step.kind == TypeKind::Object) {
            continue;
        }
        if (builtin_type_spelling(step.kind).has_value()) {
            return step;
        }
    }
    return std::nullopt;
}

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
    if (source.kind == TypeKind::Callable && target.kind == TypeKind::Class &&
        target.name == "type") {
        // This model represents a class OBJECT as its constructor Callable --
        // three sites do so: ExpressionTyper's type_of_name (`w = Widget`),
        // type_of_attribute's nested-class branch (`Outer.Inner`) and
        // type_of_name_call's bare-`C()` callee -- and `type` is the
        // annotation spelling for "some class object". Without this arm the
        // model contradicts itself: `x: type = Widget` and
        // `x: type = Outer.Inner` are both mypy-clean (measured, mypy 1.18.1)
        // yet would draw a false "incompatible types in assignment", which
        // breaks the hard invariant. The Callable/Callable arm above cannot
        // cover it (it requires the target to be a Callable too) and the
        // source.kind == Class arm below does not apply.
        //
        // Deliberately narrow: `target.name == "type"` exactly, not any
        // Class target, so a Callable still does not satisfy `x: Widget`.
        //
        // It does admit a plain function where `x: type` is wanted
        // (`def f() -> None: ...` then `x: type = f`, which mypy rejects with
        // `expression has type "Callable[[], None]"`). A deliberate MISSED
        // error, which is always safe; nothing here can tell a constructor
        // Callable apart from an ordinary function's, because the model has
        // no `type[...]` to mark one with.
        return true;
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
    // an aliased class (IOError vs. its canonical OSError) does too. For
    // EQUIVALENT inputs specifically there is no reason to prefer either
    // spelling -- both denote the same type -- so rather than literally
    // returning `left`, canonicalise each side's Class spelling (recursively,
    // via canonicalised) and then break any remaining tie with type_less, a
    // structural order over Type, NOT type_name: type_name is a diagnostics
    // spelling that is explicitly not pinned, so keying this choice on it
    // would let an unrelated rendering change silently flip which Type join
    // returns. That makes the result a function of the unordered pair
    // {left, right}, never of which argument position the caller used.
    if (is_equivalent(left, right, classes)) {
        const Type canonical_left = canonicalised(left, classes);
        const Type canonical_right = canonicalised(right, classes);
        return type_less(canonical_right, canonical_left) ? canonical_right : canonical_left;
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
