#ifndef CYTHONPP_RUNTIME_INT_H
#define CYTHONPP_RUNTIME_INT_H

#include <cstdint>

#include "bool_.h"
#include "fail.h"
#include "none.h"
#include "numeric_tag.h"

namespace py {

// Python's int, as this compiler models it.
//
// THE INTERFACE IS THE POINT, NOT THE REPRESENTATION. Emitted code names this
// type and calls the free functions below; it never spells int64_t and never
// applies a bare C++ arithmetic operator. That is what makes the
// representation swappable: a later spec may replace this with CPython's own
// small-int-plus-heap-bigint strategy, and doing so is a change to this file
// with ZERO emitter changes. Do not "simplify" the emitter by having it emit
// raw arithmetic.
//
// Overflow traps rather than wrapping. Signed overflow is undefined behaviour
// in C++, so wrapping is not merely a wrong answer, and a wrong NUMBER is the
// one failure this whole design exists to prevent. Trapping is the runtime
// analogue of the NotImplementedError the compiler reports statically: a loud
// capability claim.
//
// TASK 10B: int_ may hold a bool or a genuine int -- Python's bool is an int
// subtype, so a bool-declared value read through an int-declared name (or
// widened into one, via to_int(bool_) below) must still print and compare as
// a bool. `tag_` records which; every arithmetic function on this type
// constructs its result through the single-argument constructor, which
// defaults to Int, matching Python exactly: `True + True` is `2`, an int,
// never a bool, so arithmetic always loses the Bool tag and only
// to_int(bool_)/the two-argument constructor below ever produce one.
class int_ {
public:
    constexpr int_() = default;
    constexpr explicit int_(std::int64_t value) : value_(value) {}
    // For a value that is ACTUALLY a bool or genuinely an int -- never
    // ambiguous with the single-argument constructor above, since argument
    // counts differ; there is still exactly one single-argument (converting)
    // constructor, so the "no implicit converting constructors" rule that
    // keeps the add/sub/mul overload sets unambiguous (see float_.h) still
    // holds.
    constexpr explicit int_(std::int64_t value, NumericTag tag) : value_(value), tag_(tag) {}

    constexpr std::int64_t raw() const { return value_; }
    constexpr NumericTag tag() const { return tag_; }
    constexpr bool is_bool() const { return tag_ == NumericTag::Bool; }

private:
    std::int64_t value_ = 0;
    NumericTag tag_ = NumericTag::Int;
};

inline bool truthy(int_ v) { return v.raw() != 0; }

// Python's bool is an int subtype, so `True + 1` is `2`. The emitter inserts
// this widening around a Bool-typed operand whose operation produces an Int,
// and around a Bool-typed VALUE assigned/declared/returned where an Int is
// expected (emit_value_widened) -- the latter is why the tag must survive:
// `i: int = some_bool` must still print and compare as the bool it actually
// holds, not silently become a plain int.
inline int_ to_int(bool_ v) { return int_(v.raw() ? 1 : 0, NumericTag::Bool); }
inline int_ to_int(int_ v) { return v; }

inline int_ add(int_ a, int_ b) {
    std::int64_t result = 0;
    if (__builtin_add_overflow(a.raw(), b.raw(), &result)) {
        fail("OverflowError: integer result exceeds 64 bits");
    }
    return int_(result);
}

inline int_ sub(int_ a, int_ b) {
    std::int64_t result = 0;
    if (__builtin_sub_overflow(a.raw(), b.raw(), &result)) {
        fail("OverflowError: integer result exceeds 64 bits");
    }
    return int_(result);
}

inline int_ mul(int_ a, int_ b) {
    std::int64_t result = 0;
    if (__builtin_mul_overflow(a.raw(), b.raw(), &result)) {
        fail("OverflowError: integer result exceeds 64 bits");
    }
    return int_(result);
}

inline int_ neg(int_ a) {
    std::int64_t result = 0;
    if (__builtin_sub_overflow(static_cast<std::int64_t>(0), a.raw(), &result)) {
        fail("OverflowError: integer result exceeds 64 bits");
    }
    return int_(result);
}

// Python's unary `+` is the identity on the VALUE and NOT on the TYPE: `+True`
// is the int `1`, not `True`. So this cannot be "emit nothing", and it cannot
// be to_int() either -- to_int(bool_) deliberately PRESERVES the Bool tag (an
// annotation constrains, it does not coerce, so `x: int = True` must still
// print `True`). Shedding the tag is the whole job, and it happens here the
// same way every arithmetic result sheds it: by rebuilding through the
// single-argument constructor, which defaults to NumericTag::Int.
//
// Structurally a copy of neg() above minus the negation, deliberately, so the
// two cannot drift apart on tag handling -- `-True` is -1 and `+True` is 1,
// and both must be Int-tagged. No overflow check, because unary plus cannot
// overflow: the result is the operand.
inline int_ pos(int_ a) { return int_(a.raw()); }

// Python floors toward negative infinity; C++ truncates toward zero. Measured
// 2026-09-14: -7 // 2 is -4, and a direct transcription gives -3.
inline int_ floordiv(int_ a, int_ b) {
    if (b.raw() == 0) {
        fail("ZeroDivisionError: division by zero");
    }
    if (a.raw() == INT64_MIN && b.raw() == -1) {
        fail("OverflowError: integer result exceeds 64 bits");
    }
    std::int64_t quotient = a.raw() / b.raw();
    const std::int64_t remainder = a.raw() % b.raw();
    if (remainder != 0 && ((remainder < 0) != (b.raw() < 0))) {
        quotient -= 1;
    }
    return int_(quotient);
}

// Python's modulo takes the sign of the DIVISOR. Measured: -7 % 2 is 1, where
// C++ gives -1.
inline int_ mod(int_ a, int_ b) {
    if (b.raw() == 0) {
        fail("ZeroDivisionError: division by zero");
    }
    // Not an overflow: the mathematical result is 0 and is representable. The
    // guard exists because the C++ operator itself is UB for this pair.
    if (a.raw() == INT64_MIN && b.raw() == -1) {
        return int_(0);
    }
    std::int64_t remainder = a.raw() % b.raw();
    if (remainder != 0 && ((remainder < 0) != (b.raw() < 0))) {
        remainder += b.raw();
    }
    return int_(remainder);
}

// THE EXPONENT IS GUARANTEED NON-NEGATIVE BY THE EMITTER, which admits `**`
// only when the exponent is a bare non-negative integer literal. See the
// codegen spec's Decision 4a: a negative exponent produces a FLOAT in Python
// (2 ** -1 is 0.5) while the TypeMap says int, so emitting it at all would
// produce a silently wrong number. The guard below is a backstop against a
// future caller that forgets, not the primary defence.
inline int_ pow(int_ base, int_ exponent) {
    if (exponent.raw() < 0) {
        fail("NotImplementedError: a negative exponent is not supported");
    }
    int_ result(1);
    int_ factor = base;
    std::int64_t remaining = exponent.raw();
    while (remaining > 0) {
        if ((remaining & 1) != 0) {
            result = mul(result, factor);
        }
        remaining >>= 1;
        if (remaining > 0) {
            factor = mul(factor, factor);
        }
    }
    return result;
}

} // namespace py

#endif // CYTHONPP_RUNTIME_INT_H
