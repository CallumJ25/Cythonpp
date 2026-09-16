#ifndef CYTHONPP_RUNTIME_FLOAT_H
#define CYTHONPP_RUNTIME_FLOAT_H

#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <system_error>

#include "fail.h"
#include "int_.h"
#include "numeric_tag.h"

namespace py {

// TASK 10B: a python annotation CONSTRAINS what a name may hold, it does not
// COERCE the value -- `x: float = 1` leaves `x` holding the int `1`, not
// `1.0`, so `print(x)` must print `1`. float_ may therefore hold a bool, an
// int, or a genuine float, exactly as the numeric tower (bool <: int <:
// float) allows, and it stores whichever one it holds WITHOUT ever routing
// an integral value through `double` -- a double only exactly represents
// integers up to 2**53, and this compiler's own int64_t range reaches
// 2**63, so storing an integral value as a double would silently corrupt
// any value above 2**53 the moment it is merely held in a `float`-declared
// variable. `int_value_`/`double_value_` are two SEPARATE members rather
// than a union or std::variant purely because both are trivial scalar types
// with no destructor to manage -- a union would work identically but adds
// nothing here.
class float_ {
public:
    constexpr float_() = default;
    constexpr explicit float_(double value) : tag_(NumericTag::Float), double_value_(value) {}
    // For a value that is ACTUALLY a bool or genuinely an int, stored exactly
    // -- see the class comment above for why this can never go through
    // double_value_. Two arguments, so this can never be ambiguous with the
    // single-argument (converting) constructor above; there is still exactly
    // one single-argument constructor, preserving the "no implicit
    // converting constructors" property Task 7 required to keep the
    // add/sub/mul overload sets below unambiguous.
    constexpr explicit float_(std::int64_t value, NumericTag tag)
        : tag_(tag), int_value_(value) {}

    constexpr NumericTag tag() const { return tag_; }
    constexpr bool is_bool() const { return tag_ == NumericTag::Bool; }
    constexpr bool is_integral() const { return tag_ != NumericTag::Float; }

    // Valid only when is_integral() -- the exact value, with no precision
    // loss at all.
    //
    // FINAL-REVIEW FIX: the precondition is CHECKED, not merely documented,
    // and checked with fail() rather than assert() deliberately -- an
    // NDEBUG build strips assert, and the failure this guards is a silently
    // wrong NUMBER (a Float-tagged value's int_value_ is a leftover 0, not
    // its value), the one outcome this whole design forbids. That costs the
    // two accessors their constexpr-ness, which nothing in the runtime, the
    // emitted code, or the tests ever relied on.
    std::int64_t int_raw() const {
        if (tag_ == NumericTag::Float) {
            fail("SystemError: int_raw() on a float_ holding a real float");
        }
        return int_value_;
    }

    // The value as a double regardless of tag. For an integral tag this is a
    // CONVERTING accessor: it can lose precision above 2**53, exactly like
    // Python's own `float(some_int)`. Every arithmetic function below checks
    // is_integral()/int_raw() FIRST and only reaches this conversion once it
    // has already decided the answer needs to be a genuine float (matching
    // Python: true division, and any operation actually mixing an int with a
    // real float, are subject to the same float64 precision limit CPython
    // itself has).
    constexpr double raw() const {
        return tag_ == NumericTag::Float ? double_value_ : static_cast<double>(int_value_);
    }

    // The value as an int_, tag-preserving (Bool stays Bool). Valid only
    // when is_integral() -- checked, see int_raw() above for why with fail()
    // rather than assert().
    int_ as_int() const {
        if (tag_ == NumericTag::Float) {
            fail("SystemError: as_int() on a float_ holding a real float");
        }
        return int_(int_value_, tag_);
    }

private:
    NumericTag tag_ = NumericTag::Float;
    std::int64_t int_value_ = 0;
    double double_value_ = 0.0;
};

inline bool truthy(float_ v) { return v.is_integral() ? v.int_raw() != 0 : v.raw() != 0.0; }

inline double as_double(int_ v) { return static_cast<double>(v.raw()); }
inline double as_double(float_ v) { return v.raw(); }

// Python's numeric tower (bool <: int <: float) is a real subtyping
// relationship the checker's own is_subtype enforces -- `x: float = 1` and
// `def f(x: bool) -> float: return x` both type-check clean -- but int_ and
// bool_ have no implicit conversion to float_ (see int_.h's own comment: the
// interface is the point, and an implicit converting constructor here would
// make the add/sub/mul overload sets below ambiguous). The emitter inserts an
// explicit call to one of these three wherever a value's own type is a
// proper subtype of the C++ type it is being declared, assigned, or returned
// as. to_float(float_) is the identity case, included so a caller need not
// special-case "no widening needed" itself.
//
// TASK 10B: neither of the first two produces a lossy double any more --
// both preserve the value (and, for to_float(int_), the source's own tag)
// exactly, which is what makes `x: float = 1; print(x)` print `1`.
inline float_ to_float(bool_ v) {
    return float_(static_cast<std::int64_t>(v.raw() ? 1 : 0), NumericTag::Bool);
}
inline float_ to_float(int_ v) { return float_(v.raw(), v.tag()); }
inline float_ to_float(float_ v) { return v; }

// Python's float repr: the shortest string that round-trips, rendered fixed
// or scientific by CPython's own rule, with a trailing ".0" when the fixed
// form would otherwise be indistinguishable from an int.
//
// FINAL-REVIEW CRITICAL FIX. This used to call std::to_chars with NO format
// argument, whose "general" form chooses fixed vs scientific by SHORTEST
// CHARACTER COUNT. CPython chooses by DECIMAL EXPONENT, and the two disagree
// constantly on perfectly ordinary values: print(100000.0) printed `1e+05`
// where CPython prints `100000.0`, print(0.0001) printed `1e-04` where
// CPython prints `0.0001`, and print(123456789012345678.0) printed
// `123456789012345680.0` where CPython prints `1.2345678901234568e+17`.
// Silently wrong OUTPUT on a program both oracles accept -- the worst class
// of defect this project can ship. It survived because both pins in place at
// the time (the scalar_repr corpus sample and ReprMatchesPython) happened to
// use only values the two rules agree on.
//
// CPython's rule, from format_float_short in Python/pystrtod.c, repr mode:
// take dtoa's shortest round-trip digit string and its decimal point
// position `decpt` (the value is 0.<digits> * 10**decpt), then use
// scientific notation iff `decpt <= -4 || decpt > 16`. The 16 is deliberate
// in CPython too, with its own comment: converting at 1e17 instead gives
// odd-looking results where a 16-digit shortest repr is padded with bogus
// zeros.
//
// std::to_chars with chars_format::scientific supplies the same shortest
// round-trip digits, and its own rendering already matches Python's
// scientific spelling exactly (`1e+16`, `1.2345678901234568e+17`, `1e-05`,
// `5e-324`, at least two exponent digits and a sign) -- so the scientific
// branch returns that text verbatim and only the fixed branch reassembles.
// `decpt` is recovered as `exponent + 1`, since to_chars writes the value as
// d.ddd * 10**exponent while dtoa writes it as 0.dddd * 10**decpt.
//
// TASK 10B: this must render by TAG, not by the static C++ type -- a
// float_ tagged Int or Bool is not a "real" float at all as far as the
// Python program is concerned (it is the underlying int/bool object, merely
// reachable through a `float`-declared name), so it renders exactly as that
// int/bool would, using int_raw() rather than ever converting to double
// (which would both print the wrong spelling AND, above 2**53, the wrong
// number).
inline std::string repr(float_ v) {
    if (v.is_bool()) {
        return v.int_raw() != 0 ? "True" : "False";
    }
    if (v.is_integral()) {
        return std::to_string(v.int_raw());
    }
    const double d = v.raw();
    if (std::isnan(d)) {
        return "nan";
    }
    if (std::isinf(d)) {
        return d > 0 ? "inf" : "-inf";
    }

    char buffer[64];
    const std::to_chars_result written =
        std::to_chars(buffer, buffer + sizeof(buffer), d, std::chars_format::scientific);
    // FINAL-REVIEW FIX: the result code used to be ignored, leaving the
    // buffer's unspecified bytes to be read as a number on failure. 64 bytes
    // cannot actually be too small for a double's shortest scientific form
    // (the longest is 24 characters), so this is unreachable -- but "reads
    // uninitialised memory and prints it as a number" is not a failure mode
    // to leave resting on a size argument nobody rechecks.
    if (written.ec != std::errc()) {
        fail("SystemError: a float could not be formatted");
    }
    const std::string scientific(buffer, written.ptr);

    // Split "[-]d[.ddd]e[+-]dd" into its sign, its significant digits (with
    // the point removed) and its decimal exponent.
    std::string sign;
    std::size_t index = 0;
    if (scientific[index] == '-') {
        sign = "-";
        ++index;
    }
    std::string digits;
    int exponent = 0;
    for (; index < scientific.size(); ++index) {
        const char c = scientific[index];
        if (c == '.') {
            continue;
        }
        if (c != 'e') {
            digits += c;
            continue;
        }
        ++index;
        bool negative_exponent = false;
        if (index < scientific.size() && (scientific[index] == '+' || scientific[index] == '-')) {
            negative_exponent = scientific[index] == '-';
            ++index;
        }
        int magnitude = 0;
        for (; index < scientific.size(); ++index) {
            magnitude = magnitude * 10 + (scientific[index] - '0');
        }
        exponent = negative_exponent ? -magnitude : magnitude;
        break;
    }

    const int decpt = exponent + 1;
    if (decpt <= -4 || decpt > 16) {
        return scientific;
    }

    const int length = static_cast<int>(digits.size());
    std::string text = sign;
    if (decpt <= 0) {
        // 0.0001: digits "1", decpt -3 -> "0." + "000" + "1".
        text += "0.";
        text.append(static_cast<std::size_t>(-decpt), '0');
        text += digits;
    } else if (decpt >= length) {
        // 100000.0: digits "1", decpt 6 -> "1" + "00000" + ".0". The ".0" is
        // Python's rule that a float never renders as a bare integer.
        text += digits;
        text.append(static_cast<std::size_t>(decpt - length), '0');
        text += ".0";
    } else {
        // 12345.6789: digits "123456789", decpt 5 -> "12345" + "." + "6789".
        text += digits.substr(0, static_cast<std::size_t>(decpt));
        text += '.';
        text += digits.substr(static_cast<std::size_t>(decpt));
    }
    return text;
}

// TASK 10B, obligations 2 and 3: rank is decided from the RUNTIME tag, not
// the static py::float_ type, and when both operands are integral the
// result is computed as int64_t arithmetic (reusing int_'s own
// overflow-checked add/sub/mul, so the trap still fires) rather than as
// double -- a double add/sub/mul above 2**53 silently rounds, which is
// exactly the class of wrong-number defect this whole task exists to
// remove. `to_float(py::add(...))` both performs the checked integer
// arithmetic and re-tags the float_ result as Int (py::add(int_,int_)
// always returns an Int-tagged result, matching Python: `True + True` is
// `2`, an int, never a bool -- see int_.h). Only once at least one operand
// is a genuine float does this fall back to double arithmetic, which is
// real Python float arithmetic and therefore subject to the same float64
// precision limit CPython itself has.
inline float_ add(float_ a, float_ b) {
    if (a.is_integral() && b.is_integral()) {
        return to_float(add(a.as_int(), b.as_int()));
    }
    return float_(a.raw() + b.raw());
}
inline float_ add(float_ a, int_ b) {
    if (a.is_integral()) {
        return to_float(add(a.as_int(), b));
    }
    return float_(a.raw() + as_double(b));
}
inline float_ add(int_ a, float_ b) {
    if (b.is_integral()) {
        return to_float(add(a, b.as_int()));
    }
    return float_(as_double(a) + b.raw());
}

inline float_ sub(float_ a, float_ b) {
    if (a.is_integral() && b.is_integral()) {
        return to_float(sub(a.as_int(), b.as_int()));
    }
    return float_(a.raw() - b.raw());
}
inline float_ sub(float_ a, int_ b) {
    if (a.is_integral()) {
        return to_float(sub(a.as_int(), b));
    }
    return float_(a.raw() - as_double(b));
}
inline float_ sub(int_ a, float_ b) {
    if (b.is_integral()) {
        return to_float(sub(a, b.as_int()));
    }
    return float_(as_double(a) - b.raw());
}

inline float_ mul(float_ a, float_ b) {
    if (a.is_integral() && b.is_integral()) {
        return to_float(mul(a.as_int(), b.as_int()));
    }
    return float_(a.raw() * b.raw());
}
inline float_ mul(float_ a, int_ b) {
    if (a.is_integral()) {
        return to_float(mul(a.as_int(), b));
    }
    return float_(a.raw() * as_double(b));
}
inline float_ mul(int_ a, float_ b) {
    if (b.is_integral()) {
        return to_float(mul(a, b.as_int()));
    }
    return float_(as_double(a) * b.raw());
}

inline float_ neg(float_ a) {
    if (a.is_integral()) {
        return to_float(neg(a.as_int()));
    }
    return float_(-a.raw());
}

// The float_ arm of unary plus -- see int_.h's pos() for why `+` needs a real
// function at all rather than emitting nothing. Mirrors neg() above line for
// line, including the is_integral() branch that keeps an integral value in
// int64_t rather than routing it through double (a double is exact only to
// 2^53 while int_ reaches 2^63, so routing an integral value through raw()
// here would silently corrupt a large one).
inline float_ pos(float_ a) {
    if (a.is_integral()) {
        return to_float(pos(a.as_int()));
    }
    return float_(a.raw());
}

// Python raises for float division by zero too, where C++ yields inf. That
// makes this a check the runtime ADDS rather than one it inherits.
//
// Deliberately NOT tag-aware: Python's `/` is true division, ALWAYS a float
// result regardless of the operands' actual types (`10**18 / 1` is a float
// in real Python too, and subject to the identical float64 precision limit
// -- this is not a defect this task's model changes, since true division
// leaves the integer tower on its own terms). See int_.h's own truediv
// overload for `int / int`, which this is not: these three overloads are
// only ever reached when at least one operand's STATIC type is float_.
inline float_ truediv(double a, double b) {
    if (b == 0.0) {
        fail("ZeroDivisionError: division by zero");
    }
    return float_(a / b);
}

inline float_ truediv(int_ a, int_ b) { return truediv(as_double(a), as_double(b)); }
inline float_ truediv(float_ a, float_ b) { return truediv(a.raw(), b.raw()); }
inline float_ truediv(float_ a, int_ b) { return truediv(a.raw(), as_double(b)); }
inline float_ truediv(int_ a, float_ b) { return truediv(as_double(a), b.raw()); }

inline float_ floordiv(double a, double b) {
    if (b == 0.0) {
        fail("ZeroDivisionError: division by zero");
    }
    return float_(std::floor(a / b));
}

// TASK 10B: `//` and `%` DO propagate rank from the runtime tags, unlike
// `/` above -- Python's floor division and modulo genuinely dispatch on the
// operands' ACTUAL types, so `x // y` where a `float`-declared `x` and `y`
// both actually hold ints computes exactly as `int // int` (an int result,
// reusing int_.h's sign-correct floordiv/mod and their zero/overflow
// traps), not as a floored double.
inline float_ floordiv(float_ a, float_ b) {
    if (a.is_integral() && b.is_integral()) {
        return to_float(floordiv(a.as_int(), b.as_int()));
    }
    return floordiv(a.raw(), b.raw());
}
inline float_ floordiv(float_ a, int_ b) {
    if (a.is_integral()) {
        return to_float(floordiv(a.as_int(), b));
    }
    return floordiv(a.raw(), as_double(b));
}
inline float_ floordiv(int_ a, float_ b) {
    if (b.is_integral()) {
        return to_float(floordiv(a, b.as_int()));
    }
    return floordiv(as_double(a), b.raw());
}

inline float_ mod(double a, double b) {
    if (b == 0.0) {
        fail("ZeroDivisionError: division by zero");
    }
    // std::fmod takes the sign of the dividend; Python's takes the divisor's,
    // exactly as for int.
    double remainder = std::fmod(a, b);
    // FINAL-REVIEW FIX: a ZERO remainder takes the divisor's sign too, and
    // the `remainder != 0.0` guard skipped the correction for exactly that
    // case (0.0 == -0.0 compares equal, so the sign was whatever fmod left).
    // Measured: `7 % -0.5` is -0.0 under CPython and was 0.0 here; `-2.5 %
    // 2.5` is 0.0 and was -0.0. copysign is the whole fix, since a zero
    // remainder needs no `+= b` -- the magnitude is already right.
    if (remainder == 0.0) {
        return float_(std::copysign(0.0, b));
    }
    if ((remainder < 0.0) != (b < 0.0)) {
        remainder += b;
    }
    return float_(remainder);
}

inline float_ mod(float_ a, float_ b) {
    if (a.is_integral() && b.is_integral()) {
        return to_float(mod(a.as_int(), b.as_int()));
    }
    return mod(a.raw(), b.raw());
}
inline float_ mod(float_ a, int_ b) {
    if (a.is_integral()) {
        return to_float(mod(a.as_int(), b));
    }
    return mod(a.raw(), as_double(b));
}
inline float_ mod(int_ a, float_ b) {
    if (b.is_integral()) {
        return to_float(mod(a, b.as_int()));
    }
    return mod(as_double(a), b.raw());
}

// Integer exponent only, guaranteed by the emitter. A negative base with a
// FRACTIONAL exponent leaves the reals entirely -- measured, (-8.0) ** 0.5 is
// complex in both CPython and mypy -- and an integer exponent excludes that
// case by construction.
//
// TASK 10B: an integral-tagged base computes via int_'s own overflow-checked
// pow (matching `2 ** 3` being the int `8`, not the float `8.0`, when the
// `2` is actually an int reached through a `float`-declared name).
inline float_ pow(float_ base, int_ exponent) {
    if (exponent.raw() < 0) {
        fail("NotImplementedError: a negative exponent is not supported");
    }
    if (base.is_integral()) {
        return to_float(pow(base.as_int(), exponent));
    }
    return float_(std::pow(base.raw(), as_double(exponent)));
}

} // namespace py

#endif // CYTHONPP_RUNTIME_FLOAT_H
