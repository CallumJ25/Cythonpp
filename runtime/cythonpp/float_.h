#ifndef CYTHONPP_RUNTIME_FLOAT_H
#define CYTHONPP_RUNTIME_FLOAT_H

#include <charconv>
#include <cmath>
#include <string>

#include "fail.h"
#include "int_.h"

namespace py {

class float_ {
public:
    constexpr float_() = default;
    constexpr explicit float_(double value) : value_(value) {}

    constexpr double raw() const { return value_; }

private:
    double value_ = 0.0;
};

inline bool truthy(float_ v) { return v.raw() != 0.0; }

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
inline float_ to_float(bool_ v) { return float_(v.raw() ? 1.0 : 0.0); }
inline float_ to_float(int_ v) { return float_(as_double(v)); }
inline float_ to_float(float_ v) { return v; }

// Python's float repr: the shortest string that round-trips, with a trailing
// ".0" when the result would otherwise be indistinguishable from an int.
// std::to_chars' shortest form supplies the round-trip guarantee; the suffix
// rule supplies Python's presentation.
inline std::string repr(float_ v) {
    const double d = v.raw();
    if (std::isnan(d)) {
        return "nan";
    }
    if (std::isinf(d)) {
        return d > 0 ? "inf" : "-inf";
    }
    char buffer[40];
    const std::to_chars_result written = std::to_chars(buffer, buffer + sizeof(buffer), d);
    std::string text(buffer, written.ptr);
    if (text.find('.') == std::string::npos && text.find('e') == std::string::npos) {
        text += ".0";
    }
    return text;
}

inline float_ add(float_ a, float_ b) { return float_(a.raw() + b.raw()); }
inline float_ add(float_ a, int_ b) { return float_(a.raw() + as_double(b)); }
inline float_ add(int_ a, float_ b) { return float_(as_double(a) + b.raw()); }

inline float_ sub(float_ a, float_ b) { return float_(a.raw() - b.raw()); }
inline float_ sub(float_ a, int_ b) { return float_(a.raw() - as_double(b)); }
inline float_ sub(int_ a, float_ b) { return float_(as_double(a) - b.raw()); }

inline float_ mul(float_ a, float_ b) { return float_(a.raw() * b.raw()); }
inline float_ mul(float_ a, int_ b) { return float_(a.raw() * as_double(b)); }
inline float_ mul(int_ a, float_ b) { return float_(as_double(a) * b.raw()); }

inline float_ neg(float_ a) { return float_(-a.raw()); }

// Python raises for float division by zero too, where C++ yields inf. That
// makes this a check the runtime ADDS rather than one it inherits.
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

inline float_ floordiv(float_ a, float_ b) { return floordiv(a.raw(), b.raw()); }
inline float_ floordiv(float_ a, int_ b) { return floordiv(a.raw(), as_double(b)); }
inline float_ floordiv(int_ a, float_ b) { return floordiv(as_double(a), b.raw()); }

inline float_ mod(double a, double b) {
    if (b == 0.0) {
        fail("ZeroDivisionError: division by zero");
    }
    // std::fmod takes the sign of the dividend; Python's takes the divisor's,
    // exactly as for int.
    double remainder = std::fmod(a, b);
    if (remainder != 0.0 && ((remainder < 0.0) != (b < 0.0))) {
        remainder += b;
    }
    return float_(remainder);
}

inline float_ mod(float_ a, float_ b) { return mod(a.raw(), b.raw()); }
inline float_ mod(float_ a, int_ b) { return mod(a.raw(), as_double(b)); }
inline float_ mod(int_ a, float_ b) { return mod(as_double(a), b.raw()); }

// Integer exponent only, guaranteed by the emitter. A negative base with a
// FRACTIONAL exponent leaves the reals entirely -- measured, (-8.0) ** 0.5 is
// complex in both CPython and mypy -- and an integer exponent excludes that
// case by construction.
inline float_ pow(float_ base, int_ exponent) {
    if (exponent.raw() < 0) {
        fail("NotImplementedError: a negative exponent is not supported");
    }
    return float_(std::pow(base.raw(), as_double(exponent)));
}

} // namespace py

#endif // CYTHONPP_RUNTIME_FLOAT_H
