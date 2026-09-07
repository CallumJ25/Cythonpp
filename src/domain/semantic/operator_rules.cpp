#include "operator_rules.h"

#include <algorithm>
#include <cstddef>
#include <vector>

#include "type_compatibility.h"

namespace cythonpp::domain::semantic {
namespace {

// Bool counts as an integer: Python's bool is an int subtype, so `[1] * True`
// is list[int] and `1 & True` is int. Both verified.
bool is_integral(TypeKind kind) { return kind == TypeKind::Bool || kind == TypeKind::Int; }

Type numeric_of_rank(int rank) {
    if (rank >= 4) {
        return Type::complex_();
    }
    if (rank == 3) {
        return Type::float_();
    }
    return Type::int_();
}

// The numeric tower's join, FLOORED AT Int so two Bools widen: verified,
// reveal_type(True + True) is int.
std::optional<Type> numeric_join(const Type& left, const Type& right) {
    const int left_rank = numeric_rank(left.kind);
    const int right_rank = numeric_rank(right.kind);
    if (left_rank == 0 || right_rank == 0) {
        return std::nullopt;
    }
    return numeric_of_rank(std::max(2, std::max(left_rank, right_rank)));
}

std::optional<Type> plus_result(const Type& left, const Type& right) {
    if (const std::optional<Type> numeric = numeric_join(left, right)) {
        return numeric;
    }
    if (left.kind == TypeKind::Str && right.kind == TypeKind::Str) {
        return Type::str();
    }
    if (left.kind == TypeKind::Bytes && right.kind == TypeKind::Bytes) {
        return Type::bytes();
    }
    if (left.kind == TypeKind::ByteArray && right.kind == TypeKind::ByteArray) {
        return Type::bytearray_();
    }
    if (left.kind == TypeKind::List && right.kind == TypeKind::List && is_equivalent(left, right)) {
        // Equivalent, not ==, matching list's invariance the way is_subtype
        // now does: list[int | str] and list[str | int] are the same type
        // even though they are not ==, and there is still no join that would
        // be sound for a mutable container beyond that.
        return left;
    }
    if (left.kind == TypeKind::Tuple && right.kind == TypeKind::Tuple) {
        std::vector<Type> elements = left.args;
        for (const Type& element : right.args) {
            elements.push_back(element);
        }
        return Type::tuple_of(std::move(elements));
    }
    return std::nullopt;
}

std::optional<Type> star_result(const Type& left, const Type& right) {
    if (const std::optional<Type> numeric = numeric_join(left, right)) {
        return numeric;
    }
    // Repetition. numeric_join already returned if BOTH were numeric, so at
    // most one side is integral here and this picks the sequence unambiguously.
    const Type& sequence = is_integral(left.kind) ? right : left;
    const Type& count = is_integral(left.kind) ? left : right;
    if (!is_integral(count.kind)) {
        return std::nullopt;
    }
    if (sequence.kind == TypeKind::Str || sequence.kind == TypeKind::Bytes ||
        sequence.kind == TypeKind::ByteArray || sequence.kind == TypeKind::List) {
        return sequence;
    }
    // Tuple is deliberately ABSENT here, and this is not an omission: mypy
    // types `t * n` for a non-literal count as tuple[int, ...], a VARIADIC
    // tuple this model cannot represent (Type has no variadic-tuple
    // constructor, and the spec rejects tuple[int, ...] annotations
    // outright). A concrete fixed-arity answer would be a wrong type, which
    // is worse than a missing one, so this stays nullopt -- a "cannot model"
    // gap, not a type error.
    return std::nullopt;
}

std::optional<Type> true_divide_result(const Type& left, const Type& right) {
    const std::optional<Type> numeric = numeric_join(left, right);
    if (!numeric) {
        return std::nullopt;
    }
    // Python 3's `/` is ALWAYS true division, so int / int is float. The
    // tower's join is consulted only to decide whether complex is involved.
    return numeric->kind == TypeKind::Complex ? Type::complex_() : Type::float_();
}

std::optional<Type> modulo_result(const Type& left, const Type& right) {
    if (const std::optional<Type> numeric = numeric_join(left, right)) {
        return numeric;
    }
    // printf-style formatting. The format string's placeholders are not
    // checked against the right operand -- that is mypy's str-format rule,
    // which the spec puts out of scope -- so anything is accepted there.
    if (left.kind == TypeKind::Str) {
        return Type::str();
    }
    // ByteArray joins the Bytes arm here, NOT a bytearray arm of its own.
    // Verified against mypy 1.18.1: `reveal_type(bytearray(b"x") % 3)` is
    // "builtins.bytes", not bytearray -- confirmed by also checking
    // `y: bytearray = bytearray(b"x") % 3`, which mypy --strict rejects as
    // incompatible ("bytes" vs "bytearray"). This is typeshed's stub for
    // bytearray.__mod__, and it disagrees with the real CPython runtime
    // (bytearray % anything IS a bytearray there) -- but this compiler's
    // contract is mypy-compliance, not runtime fidelity. Modelling this arm
    // as returning bytearray, as an earlier draft of this fix assumed, would
    // make `z: bytes = ba % 3` a false TypeError on code mypy accepts
    // cleanly, which is exactly the invariant this wave exists to protect.
    if (left.kind == TypeKind::Bytes || left.kind == TypeKind::ByteArray) {
        return Type::bytes();
    }
    return std::nullopt;
}

// & and | are integer bitwise operators and also set/frozenset
// intersection/union. ^ on sets is a recorded gap: only & and | are modelled.
std::optional<Type> intersection_or_union_result(const Type& left, const Type& right) {
    if (is_integral(left.kind) && is_integral(right.kind)) {
        return Type::int_();
    }
    if ((left.kind == TypeKind::Set && right.kind == TypeKind::Set) ||
        (left.kind == TypeKind::FrozenSet && right.kind == TypeKind::FrozenSet)) {
        // Equivalent, not ==, for the same reason as plus_result's List arm:
        // set[int | str] and set[str | int] are the same type.
        if (is_equivalent(left, right)) {
            return left;
        }
    }
    return std::nullopt;
}

std::optional<Type> integer_only_result(const Type& left, const Type& right) {
    if (is_integral(left.kind) && is_integral(right.kind)) {
        return Type::int_();
    }
    return std::nullopt;
}

// Everything `in` accepts on its right and `for` can iterate. ByteArray is
// included even though the spec's table omits it: a bytearray is iterable and
// a container in Python, and its absence reads as an oversight rather than a
// decision. Adding it can only accept more, never report more, so the hard
// invariant is untouched.
bool is_container(TypeKind kind) {
    return kind == TypeKind::List || kind == TypeKind::Dict || kind == TypeKind::Set ||
           kind == TypeKind::FrozenSet || kind == TypeKind::Tuple || kind == TypeKind::Str ||
           kind == TypeKind::Bytes || kind == TypeKind::ByteArray || kind == TypeKind::Range;
}

std::optional<Type> ordered_result(const Type& left, const Type& right) {
    // Bool rather than a report: the root cause already reported, and a
    // comparison's result type never depends on its operands.
    if (left.kind == TypeKind::Unknown || right.kind == TypeKind::Unknown) {
        return Type::bool_();
    }
    if (numeric_rank(left.kind) != 0 && numeric_rank(right.kind) != 0) {
        return Type::bool_();
    }
    if (left.kind != right.kind) {
        return std::nullopt;
    }
    if (left.kind == TypeKind::Str || left.kind == TypeKind::Bytes ||
        left.kind == TypeKind::ByteArray) {
        return Type::bool_();
    }
    if (left.kind == TypeKind::List && is_equivalent(left, right)) {
        // Equivalent, not ==, for the same reason as plus_result's List arm.
        return Type::bool_();
    }
    if (left.kind == TypeKind::Tuple) {
        // Elementwise at runtime, and heterogeneous tuples compare fine, so
        // the element types are not constrained here.
        return Type::bool_();
    }
    // Sets support subset comparison in Python (`s1 <= s2`); not modelled,
    // a recorded gap -- verified against mypy 1.18.1, which types it bool
    // with no error. Left nullopt because this compiler's table has no way
    // to distinguish "cannot model" from "type error"; see the same
    // observation at plus_result/star_result's tuple-repetition gap.
    return std::nullopt;
}

} // namespace

std::optional<Type> binary_result(lexer::token_type op, const Type& left, const Type& right) {
    // Absorbing, and before the operator switch so it also covers operators
    // that would otherwise never apply: the root cause already reported.
    if (left.kind == TypeKind::Unknown || right.kind == TypeKind::Unknown) {
        return Type::unknown();
    }

    switch (op) {
    case lexer::token_type::OP_PLUS:
        return plus_result(left, right);
    case lexer::token_type::OP_MINUS:
        return numeric_join(left, right);
    case lexer::token_type::OP_STAR:
        return star_result(left, right);
    case lexer::token_type::OP_SLASH:
        return true_divide_result(left, right);
    case lexer::token_type::OP_DOUBLE_SLASH:
        // Complex // complex is accepted here. mypy rejects it; a missed
        // error rather than a false one, so the hard invariant holds.
        return numeric_join(left, right);
    case lexer::token_type::OP_DOUBLE_STAR:
        // int ** int is int. mypy types 2 ** -1 as float by reading the
        // literal's sign, which needs literal types. A recorded gap.
        return numeric_join(left, right);
    case lexer::token_type::OP_PERCENT:
        return modulo_result(left, right);
    case lexer::token_type::OP_AT:
        // Matrix multiplication: no builtin type supports it, so this never
        // applies. The semantic half of OP_AT's two readings.
        return std::nullopt;
    case lexer::token_type::OP_AMPERSAND:
    case lexer::token_type::OP_PIPE:
        return intersection_or_union_result(left, right);
    case lexer::token_type::OP_CARET:
    case lexer::token_type::OP_LEFT_SHIFT:
    case lexer::token_type::OP_RIGHT_SHIFT:
        return integer_only_result(left, right);
    default:
        // Not a binary arithmetic or bitwise operator. A default is right in
        // a switch over token_type, which has well over a hundred
        // enumerators; Global Constraint 5 forbids one only over TypeKind.
        return std::nullopt;
    }
}

std::optional<Type> unary_result(lexer::token_type op, const Type& operand) {
    // BEFORE the Unknown guard, deliberately. Truthiness is universal in
    // Python, so `not` is total and always yields bool; absorbing Unknown
    // here would turn Bool into Unknown and silence a genuine error
    // downstream.
    if (op == lexer::token_type::OP_NOT) {
        return Type::bool_();
    }
    if (operand.kind == TypeKind::Unknown) {
        return Type::unknown();
    }

    switch (op) {
    case lexer::token_type::OP_PLUS:
    case lexer::token_type::OP_MINUS: {
        const int rank = numeric_rank(operand.kind);
        if (rank == 0) {
            return std::nullopt;
        }
        // Floored at Int, so -True is int, consistent with the binary rules.
        return numeric_of_rank(std::max(2, rank));
    }
    case lexer::token_type::OP_TILDE:
        if (is_integral(operand.kind)) {
            return Type::int_();
        }
        return std::nullopt;
    default:
        return std::nullopt;
    }
}

std::optional<Type> comparison_result(lexer::token_type op, const Type& left, const Type& right) {
    switch (op) {
    case lexer::token_type::OP_EQUAL:
    case lexer::token_type::OP_NOT_EQUAL:
    case lexer::token_type::OP_IS:
    case lexer::token_type::OP_IS_NOT:
        // Total: any operands, always bool, never a report. Whether the
        // operands could ever be equal is mypy's strict-equality rule, which
        // the spec puts out of scope. Note this deliberately does NOT absorb
        // Unknown -- the result type does not depend on the operands.
        return Type::bool_();
    case lexer::token_type::OP_IN:
    case lexer::token_type::OP_NOT_IN:
        if (right.kind == TypeKind::Unknown || is_container(right.kind)) {
            return Type::bool_();
        }
        return std::nullopt;
    case lexer::token_type::OP_LESS:
    case lexer::token_type::OP_LESS_EQUAL:
    case lexer::token_type::OP_GREATER:
    case lexer::token_type::OP_GREATER_EQUAL:
        return ordered_result(left, right);
    default:
        return std::nullopt;
    }
}

std::optional<Type> subscript_result(const Type& container, const Type& index,
                                     const ClassLookup* classes) {
    if (container.kind == TypeKind::Unknown || index.kind == TypeKind::Unknown) {
        return Type::unknown();
    }

    // Dict first, because it is the one container whose index is not an
    // integer and so must skip the integral check below.
    if (container.kind == TypeKind::Dict) {
        if (container.args.size() != 2) {
            return std::nullopt;
        }
        // By assignability, not equality, so a dict keyed by a base class
        // accepts a subclass index and the numeric tower applies to the key.
        // This is the only reason this function takes a ClassLookup.
        if (!is_subtype(index, container.args.front(), classes)) {
            return std::nullopt;
        }
        return container.args.back();
    }

    if (!is_integral(index.kind)) {
        return std::nullopt;
    }
    if (container.kind == TypeKind::List) {
        if (container.args.empty()) {
            return std::nullopt;
        }
        return container.args.front();
    }
    if (container.kind == TypeKind::Str) {
        return Type::str();
    }
    if (container.kind == TypeKind::Bytes || container.kind == TypeKind::ByteArray ||
        container.kind == TypeKind::Range) {
        // Indexing bytes yields an int, not a bytes. Verified.
        return Type::int_();
    }
    if (container.kind == TypeKind::Tuple) {
        // BEFORE consulting union_of: an empty tuple (`tuple[()]`) has no
        // element to index, and nothing upstream reported that -- resolve_
        // subscript accepts `tuple[()]` silently. union_of({}) is Unknown,
        // which is the ABSORBING bottom for an already-reported error; here
        // no error was ever reported, so returning it would silently accept
        // `t[0]` on an empty tuple and then propagate Unknown through every
        // later use, masking real errors downstream instead of surfacing
        // this one. nullopt is the caller's cue to report instead.
        if (container.args.empty()) {
            return std::nullopt;
        }
        // The union of every member. mypy selects the one member a LITERAL
        // index names, which needs literal types; the union is the sound
        // approximation, and is exactly what mypy produces for a variable
        // index. union_of de-duplicates, so a homogeneous tuple collapses.
        return Type::union_of(container.args);
    }
    return std::nullopt;
}

std::optional<Type> element_type(const Type& iterable) {
    if (iterable.kind == TypeKind::Unknown) {
        return Type::unknown();
    }
    if (iterable.kind == TypeKind::List || iterable.kind == TypeKind::Set ||
        iterable.kind == TypeKind::FrozenSet || iterable.kind == TypeKind::Dict) {
        // Iterating a dict yields its KEYS, not its items -- which is why
        // args.front() is right for all four of these.
        if (iterable.args.empty()) {
            return std::nullopt;
        }
        return iterable.args.front();
    }
    if (iterable.kind == TypeKind::Str) {
        return Type::str();
    }
    if (iterable.kind == TypeKind::Bytes || iterable.kind == TypeKind::ByteArray ||
        iterable.kind == TypeKind::Range) {
        return Type::int_();
    }
    if (iterable.kind == TypeKind::Tuple) {
        // Same reasoning as subscript_result's Tuple arm: an empty tuple has
        // no element to yield and nothing reported that, so this must not
        // silently hand back the absorbing Unknown -- `for v in ()` would
        // otherwise bind v: Unknown and silence every genuine error in the
        // loop body.
        if (iterable.args.empty()) {
            return std::nullopt;
        }
        return Type::union_of(iterable.args);
    }
    return std::nullopt;
}

} // namespace cythonpp::domain::semantic
