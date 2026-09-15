#ifndef CYTHONPP_RUNTIME_COMPARE_H
#define CYTHONPP_RUNTIME_COMPARE_H

#include <cmath>
#include <cstdint>

#include "bool_.h"
#include "float_.h"
#include "int_.h"
#include "str.h"

namespace py {

// Comparison yields Python's bool, not C++'s, so a comparison result prints
// as True/False and composes with the rest of the model.
//
// Defined as templates over the runtime's own wrapper types for every pair
// whose `.raw()` values compare correctly with a bare C++ operator: same-type
// pairs (str included, per the byte-order property str.h documents), and any
// pair involving bool_ (0/1 always converts exactly into either int64_t or
// double, so no precision is ever at stake).
//
// TASK 10B: the one place a bare `.raw()` comparison is NOT safe is int_
// against float_ (either order), and, within float_ itself, an
// INTEGRAL-tagged float_ against anything -- converting a large int64_t to
// double to compare can round it near the precision limit (obligation 5).
// Those get dedicated, exact, non-template overloads below; a non-template
// overload always wins overload resolution against this template for an
// exact-type match ([over.match.best]), so adding them here does not need
// touching a single call site.
template <typename A, typename B>
inline bool_ lt(const A& a, const B& b) { return bool_(a.raw() < b.raw()); }
template <typename A, typename B>
inline bool_ le(const A& a, const B& b) { return bool_(a.raw() <= b.raw()); }
template <typename A, typename B>
inline bool_ gt(const A& a, const B& b) { return bool_(a.raw() > b.raw()); }
template <typename A, typename B>
inline bool_ ge(const A& a, const B& b) { return bool_(a.raw() >= b.raw()); }
template <typename A, typename B>
inline bool_ eq(const A& a, const B& b) { return bool_(a.raw() == b.raw()); }
template <typename A, typename B>
inline bool_ ne(const A& a, const B& b) { return bool_(a.raw() != b.raw()); }

// Exact int64_t-vs-double comparison with no precision-losing conversion in
// either direction. -1/0/1 is the ordinary three-way result; 2 is the
// "unordered" case IEEE 754 needs for NaN (Python: `5 < float("nan")` is
// False, and so is every other ordered comparison, while `!=` is True) --
// every caller below derives its bool_ from this one four-way result so the
// NaN rule only has to be stated once.
//
// d is compared against 2**63 first because that is exactly one past
// INT64_MAX, so `d >= kTwoPow63` means `i < d` unconditionally and
// `d < -kTwoPow63` means `i > d` unconditionally -- both decided without
// ever converting i to double. Once d is known to sit inside
// [-2**63, 2**63), std::floor(d) converts back to int64_t exactly: a
// double's 53-bit mantissa cannot represent a fractional part once
// |d| >= 2**53, so at that magnitude d is ALREADY a whole number and
// floor(d) == d; below that magnitude floor(d) is an ordinary, exact
// integer truncation. Either way floor_i is exact, so comparing it against i
// is exact, and the fractional remainder (only ever nonzero when
// |d| < 2**53) is what breaks a tie in favour of d being the larger value.
inline int compare_int64_and_double(std::int64_t i, double d) {
    if (std::isnan(d)) {
        return 2;
    }
    constexpr double kTwoPow63 = 9223372036854775808.0; // 2**63, one past INT64_MAX
    if (d >= kTwoPow63) {
        return -1;
    }
    if (d < -kTwoPow63) {
        return 1;
    }
    const double floor_d = std::floor(d);
    const std::int64_t floor_i = static_cast<std::int64_t>(floor_d);
    if (i < floor_i) {
        return -1;
    }
    if (i > floor_i) {
        return 1;
    }
    return d > floor_d ? -1 : 0;
}

// Compares an int64_t against a float_ of ANY tag. When f is itself
// integral, this is an exact int64-vs-int64 comparison and never touches
// double at all, so a float_ holding a value past 2**53 (see float_.h)
// compares exactly against an int_.
inline int compare_int64_and_float(std::int64_t i, float_ f) {
    if (f.is_integral()) {
        const std::int64_t j = f.int_raw();
        return i < j ? -1 : (i > j ? 1 : 0);
    }
    return compare_int64_and_double(i, f.raw());
}

inline bool_ lt(int_ a, float_ b) { return bool_(compare_int64_and_float(a.raw(), b) == -1); }
inline bool_ le(int_ a, float_ b) {
    const int c = compare_int64_and_float(a.raw(), b);
    return bool_(c == -1 || c == 0);
}
inline bool_ gt(int_ a, float_ b) { return bool_(compare_int64_and_float(a.raw(), b) == 1); }
inline bool_ ge(int_ a, float_ b) {
    const int c = compare_int64_and_float(a.raw(), b);
    return bool_(c == 1 || c == 0);
}
inline bool_ eq(int_ a, float_ b) { return bool_(compare_int64_and_float(a.raw(), b) == 0); }
inline bool_ ne(int_ a, float_ b) { return bool_(compare_int64_and_float(a.raw(), b) != 0); }

// Mirror image of the six above: same helper, comparison flipped.
inline bool_ lt(float_ a, int_ b) { return bool_(compare_int64_and_float(b.raw(), a) == 1); }
inline bool_ le(float_ a, int_ b) {
    const int c = compare_int64_and_float(b.raw(), a);
    return bool_(c == 1 || c == 0);
}
inline bool_ gt(float_ a, int_ b) { return bool_(compare_int64_and_float(b.raw(), a) == -1); }
inline bool_ ge(float_ a, int_ b) {
    const int c = compare_int64_and_float(b.raw(), a);
    return bool_(c == -1 || c == 0);
}
inline bool_ eq(float_ a, int_ b) { return bool_(compare_int64_and_float(b.raw(), a) == 0); }
inline bool_ ne(float_ a, int_ b) { return bool_(compare_int64_and_float(b.raw(), a) != 0); }

// float_ against float_: both sides may independently be integral or
// genuinely real, so all combinations are handled explicitly rather than
// falling through to the generic template's bare `.raw() < .raw()`, which
// would silently reintroduce the int64-through-double rounding this file
// exists to avoid whenever EITHER side is integral.
inline int compare_float_and_float(float_ a, float_ b) {
    if (a.is_integral() && b.is_integral()) {
        const std::int64_t x = a.int_raw();
        const std::int64_t y = b.int_raw();
        return x < y ? -1 : (x > y ? 1 : 0);
    }
    if (a.is_integral()) {
        return compare_int64_and_float(a.int_raw(), b);
    }
    if (b.is_integral()) {
        const int c = compare_int64_and_float(b.int_raw(), a); // b vs a
        return c == 2 ? 2 : -c;
    }
    const double x = a.raw();
    const double y = b.raw();
    if (x < y) {
        return -1;
    }
    if (x > y) {
        return 1;
    }
    if (x == y) {
        return 0;
    }
    return 2; // NaN on one or both sides.
}

inline bool_ lt(float_ a, float_ b) { return bool_(compare_float_and_float(a, b) == -1); }
inline bool_ le(float_ a, float_ b) {
    const int c = compare_float_and_float(a, b);
    return bool_(c == -1 || c == 0);
}
inline bool_ gt(float_ a, float_ b) { return bool_(compare_float_and_float(a, b) == 1); }
inline bool_ ge(float_ a, float_ b) {
    const int c = compare_float_and_float(a, b);
    return bool_(c == 1 || c == 0);
}
inline bool_ eq(float_ a, float_ b) { return bool_(compare_float_and_float(a, b) == 0); }
inline bool_ ne(float_ a, float_ b) { return bool_(compare_float_and_float(a, b) != 0); }

} // namespace py

#endif // CYTHONPP_RUNTIME_COMPARE_H
