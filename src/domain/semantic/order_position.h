#ifndef CYTHONPP_DOMAIN_SEMANTIC_ORDER_POSITION_H
#define CYTHONPP_DOMAIN_SEMANTIC_ORDER_POSITION_H

#include <limits>

namespace cythonpp::domain::semantic {

// WHERE something sits in its scope's execution order, for the ordering rule
// (ExpressionTyper::type_of_name: a read is a use-before-definition when the
// binding it resolves to is positioned at or after the reading statement).
//
// A DISTINCT TYPE rather than a bare `int`, and the reason is not style. This
// value is a SOURCE LINE today and is NOT required to stay one -- two
// statements sharing a physical line via `;` are indistinguishable to a
// line-based comparison, which is a recorded open false positive
// (`x = 5; print(x)` is mypy-clean and CPython-clean, and reported here), and
// closing it means this becoming a statement ORDINAL instead. Meanwhile
// Binding::declared_at, which used to be the SAME `int`, is genuinely a line
// forever: it is printed in `already defined on line N` and must keep naming
// a place a reader can look at.
//
// So the two are on diverging trajectories, and while both were one `int`
// field they were repeatedly conflated -- see Binding::declared_at's own
// comment for the measured regression that cost (`3b83611`, fourteen shapes
// where a real `[no-redef]` was deleted). Splitting them into two `int`s
// prevents THAT bug; making one of them a type the other cannot be assigned
// to is what stops the same conflation reappearing the moment this stops
// being a line. The project already spends exactly this ceremony where a
// silent wrong answer is the failure mode: `-Werror=switch` with no
// `default:`, DiagnosticSink::report's mandatory Suppressibility,
// CodegenMode's deliberately-absent default.
//
// NO implicit conversion in either direction, deliberately. `line()` is
// explicit and exists for the two places that genuinely still need the
// underlying number; every comparison goes through this type's own
// operators, so a future change of representation is a compile error at each
// site rather than a silently different answer.
class OrderPosition {
public:
    // UNSET, spelled 0 to match what Binding::loop_start_line has always
    // used for "this binding is not inside any loop". Tested with is_set(),
    // never by comparing against a bare literal -- the magic `!= 0` at the
    // reader is part of what this type exists to remove.
    constexpr OrderPosition() = default;

    static constexpr OrderPosition at_line(int line) { return OrderPosition(line); }

    static constexpr OrderPosition unset() { return OrderPosition(); }

    // A position AFTER every real one, so `binding >= this` is false for any
    // real binding and the ordering check can never fire. This is
    // ExpressionTyper::statement_line_'s default: a unit test that never
    // calls set_statement_line must not have the ordering rule applied to
    // it, and every such test predates the rule. See statement_line_'s own
    // comment -- the sentinel is opt-OUT by construction rather than
    // something each test remembers to disable.
    static constexpr OrderPosition after_all() {
        return OrderPosition(std::numeric_limits<int>::max());
    }

    constexpr bool is_set() const { return value_ != kUnset; }

    // The underlying line. Explicit, and deliberately not an operator: a
    // caller reaching for this is stepping outside the ordering abstraction
    // and should be visible when this type stops being a line.
    constexpr int line() const { return value_; }

    friend constexpr bool operator==(OrderPosition left, OrderPosition right) {
        return left.value_ == right.value_;
    }
    friend constexpr bool operator!=(OrderPosition left, OrderPosition right) {
        return !(left == right);
    }
    friend constexpr bool operator<(OrderPosition left, OrderPosition right) {
        return left.value_ < right.value_;
    }
    friend constexpr bool operator>(OrderPosition left, OrderPosition right) {
        return right < left;
    }
    friend constexpr bool operator<=(OrderPosition left, OrderPosition right) {
        return !(right < left);
    }
    friend constexpr bool operator>=(OrderPosition left, OrderPosition right) {
        return !(left < right);
    }

private:
    explicit constexpr OrderPosition(int value) : value_(value) {}

    static constexpr int kUnset = 0;

    int value_ = kUnset;
};

} // namespace cythonpp::domain::semantic

#endif // CYTHONPP_DOMAIN_SEMANTIC_ORDER_POSITION_H
