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

// TASK 10B, obligation 1: a python annotation constrains what a name may
// hold, it does not coerce the value -- `x: float = 1` leaves `x` holding
// the int `1`, so it must both print AND repr as an int, not as "3.0".
TEST(RuntimeFloat, ToFloatWidensIntWithoutCoercingItToADouble) {
    const py::float_ widened = py::to_float(py::int_(3));
    EXPECT_TRUE(widened.is_integral());
    EXPECT_FALSE(widened.is_bool());
    EXPECT_EQ(widened.int_raw(), 3);
    EXPECT_EQ(py::repr(widened), "3");
}

// Symmetric case for a bool: to_float(bool_) must render as True/False, not
// as a number at all.
TEST(RuntimeFloat, ToFloatWidensBoolWithoutCoercingItToANumber) {
    const py::float_ widened_true = py::to_float(py::bool_(true));
    EXPECT_TRUE(widened_true.is_bool());
    EXPECT_EQ(py::repr(widened_true), "True");

    const py::float_ widened_false = py::to_float(py::bool_(false));
    EXPECT_TRUE(widened_false.is_bool());
    EXPECT_EQ(py::repr(widened_false), "False");
}

// TASK 10B, obligation 2/3: `int + int` stays an int -- computed in int64_t,
// not double -- even when both operands are held in float_-typed values.
TEST(RuntimeFloat, ArithmeticOnTwoIntegralFloatsStaysIntegral) {
    const py::float_ a = py::to_float(py::int_(2));
    const py::float_ b = py::to_float(py::int_(3));

    const py::float_ sum = py::add(a, b);
    EXPECT_TRUE(sum.is_integral());
    EXPECT_EQ(sum.int_raw(), 5);
    EXPECT_EQ(py::repr(sum), "5");

    EXPECT_TRUE(py::sub(a, b).is_integral());
    EXPECT_EQ(py::sub(a, b).int_raw(), -1);
    EXPECT_TRUE(py::mul(a, b).is_integral());
    EXPECT_EQ(py::mul(a, b).int_raw(), 6);
    EXPECT_TRUE(py::neg(a).is_integral());
    EXPECT_EQ(py::neg(a).int_raw(), -2);
}

// `10**18` is exactly representable as a double, but `10**18 + 1` is not --
// this is the corpus sample's own defect class at unit-test granularity, and
// the reason obligation 3 forbids computing float_ arithmetic in double and
// converting back.
TEST(RuntimeFloat, IntegralFloatArithmeticStaysPreciseAbove2Pow53) {
    const py::float_ x = py::to_float(py::int_(1000000000000000000LL)); // 10**18
    const py::float_ y = py::add(x, py::int_(1));
    EXPECT_TRUE(y.is_integral());
    EXPECT_EQ(y.int_raw(), 1000000000000000001LL);
    EXPECT_EQ(py::repr(y), "1000000000000000001");
    // The naive (wrong) implementation this task replaces would have gone
    // through double and printed something else -- pin that double actually
    // cannot tell these two values apart, so the fix is provably load-bearing.
    EXPECT_EQ(static_cast<double>(1000000000000000000LL),
              static_cast<double>(1000000000000000001LL));
}

// TASK 10B, obligation 2: mixing an integral float_ with a genuine one
// produces a genuine (real) result -- rank still joins to the higher one.
TEST(RuntimeFloat, MixingIntegralAndRealFloatProducesARealResult) {
    const py::float_ integral = py::to_float(py::int_(2));
    const py::float_ real = py::float_(0.5);

    const py::float_ sum = py::add(integral, real);
    EXPECT_FALSE(sum.is_integral());
    EXPECT_EQ(sum.raw(), 2.5);
    EXPECT_EQ(py::repr(sum), "2.5");
}

// TASK 10B, obligation 3: the overflow trap still fires for arithmetic on
// two integral float_ values, exactly as it does for plain int_.
TEST(RuntimeFloatDeathTest, IntegralOverflowInsideAFloatStillTraps) {
    const py::float_ near_max = py::to_float(py::int_(INT64_MAX));
    EXPECT_EXIT(py::add(near_max, py::int_(1)), ::testing::ExitedWithCode(1), "OverflowError");
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
