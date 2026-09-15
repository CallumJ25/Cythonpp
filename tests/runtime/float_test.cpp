#include <string>

#include <gtest/gtest.h>

#include "cythonpp/float_.h"

namespace {

// Python's repr is shortest-round-trip, and always shows a decimal point or
// an exponent so a float is never mistaken for an int. C++'s default
// operator<< prints 1.0 as "1".
TEST(RuntimeFloat, ReprMatchesPython) {
    EXPECT_EQ(py::repr(py::float_(1.0)), "1.0");
    EXPECT_EQ(py::repr(py::float_(0.1)), "0.1");
    EXPECT_EQ(py::repr(py::float_(3.5)), "3.5");
    EXPECT_EQ(py::repr(py::float_(-512.0)), "-512.0");
    EXPECT_EQ(py::repr(py::float_(1e300)), "1e+300");
    EXPECT_EQ(py::repr(py::float_(1.0 / 3.0)), "0.3333333333333333");
}

TEST(RuntimeFloat, TrueDivisionAlwaysProducesFloat) {
    EXPECT_EQ(py::truediv(py::int_(7), py::int_(2)).raw(), 3.5);
    EXPECT_EQ(py::repr(py::truediv(py::int_(1), py::int_(3))), "0.3333333333333333");
}

// The numeric-tower widening the emitter inserts for a `bool`/`int` value
// assigned, declared, or returned where a `float` is expected -- see
// emitter_statements.cpp's emit_value_widened. to_float(float_) is the
// identity case a caller reaches when no widening was actually necessary.
TEST(RuntimeFloat, ToFloatWidensBoolAndInt) {
    EXPECT_EQ(py::to_float(py::bool_(true)).raw(), 1.0);
    EXPECT_EQ(py::to_float(py::bool_(false)).raw(), 0.0);
    EXPECT_EQ(py::to_float(py::int_(7)).raw(), 7.0);
    EXPECT_EQ(py::to_float(py::float_(2.5)).raw(), 2.5);
}

TEST(RuntimeFloat, MixedIntAndFloatArithmetic) {
    EXPECT_EQ(py::add(py::int_(1), py::float_(0.5)).raw(), 1.5);
    EXPECT_EQ(py::add(py::float_(0.5), py::int_(1)).raw(), 1.5);
    EXPECT_EQ(py::mul(py::float_(2.0), py::float_(3.0)).raw(), 6.0);
}

// A float base with an integer exponent stays real; only a FRACTIONAL
// exponent on a negative base leaves the reals, and the emitter never admits
// one. Measured: (-8.0) ** 3 is -512.0.
TEST(RuntimeFloat, PowWithIntegerExponent) {
    EXPECT_EQ(py::pow(py::float_(-8.0), py::int_(3)).raw(), -512.0);
    EXPECT_EQ(py::pow(py::float_(-8.0), py::int_(0)).raw(), 1.0);
}

// C++ yields inf here rather than faulting; Python raises. This is the trap
// that makes float division-by-zero a check the runtime must ADD.
TEST(RuntimeFloatDeathTest, FloatDivisionByZeroExitsNonZero) {
    EXPECT_EXIT(py::truediv(py::float_(1.0), py::float_(0.0)),
                ::testing::ExitedWithCode(1), "ZeroDivisionError");
    EXPECT_EXIT(py::floordiv(py::float_(1.0), py::float_(0.0)),
                ::testing::ExitedWithCode(1), "ZeroDivisionError");
}

} // namespace
