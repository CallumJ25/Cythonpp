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
    if (left.kind == TypeKind::List && right.kind == TypeKind::List && left == right) {
        // Exact element match, matching list's invariance: there is no join
        // that would be sound for a mutable container.
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
        sequence.kind == TypeKind::List) {
        return sequence;
    }
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
    if (left.kind == TypeKind::Bytes) {
        return Type::bytes();
    }
    return std::nullopt;
}

// & and | are integer bitwise operators and also set intersection/union.
// ^ on sets is a recorded gap: only & and | are modelled.
std::optional<Type> intersection_or_union_result(const Type& left, const Type& right) {
    if (is_integral(left.kind) && is_integral(right.kind)) {
        return Type::int_();
    }
    if (left.kind == TypeKind::Set && right.kind == TypeKind::Set && left == right) {
        return left;
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
    if (left.kind == TypeKind::List && left == right) {
        return Type::bool_();
    }
    if (left.kind == TypeKind::Tuple) {
        // Elementwise at runtime, and heterogeneous tuples compare fine, so
        // the element types are not constrained here.
        return Type::bool_();
    }
    // Sets support subset comparison in Python; not modelled, a recorded gap.
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

} // namespace cythonpp::domain::semantic
