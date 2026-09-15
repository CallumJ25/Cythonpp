#include <gtest/gtest.h>

#include "cythonpp/int_.h"

namespace {

TEST(RuntimeInt, AddSubMul) {
    EXPECT_EQ(py::add(py::int_(2), py::int_(3)).raw(), 5);
    EXPECT_EQ(py::sub(py::int_(2), py::int_(3)).raw(), -1);
    EXPECT_EQ(py::mul(py::int_(2), py::int_(3)).raw(), 6);
    EXPECT_EQ(py::neg(py::int_(7)).raw(), -7);
}

// Python floors; C++ truncates. -7 // 2 is -4 in Python and -3 if the C++
// operator is transcribed directly.
TEST(RuntimeInt, FloorDivisionFloorsTowardNegativeInfinity) {
    EXPECT_EQ(py::floordiv(py::int_(-7), py::int_(2)).raw(), -4);
    EXPECT_EQ(py::floordiv(py::int_(7), py::int_(-2)).raw(), -4);
    EXPECT_EQ(py::floordiv(py::int_(7), py::int_(2)).raw(), 3);
    EXPECT_EQ(py::floordiv(py::int_(-7), py::int_(-2)).raw(), 3);
}

// Python's modulo takes the sign of the DIVISOR; C++'s takes the sign of the
// dividend. -7 % 2 is 1 in Python and -1 in C++.
TEST(RuntimeInt, ModuloSignFollowsTheDivisor) {
    EXPECT_EQ(py::mod(py::int_(-7), py::int_(2)).raw(), 1);
    EXPECT_EQ(py::mod(py::int_(7), py::int_(-2)).raw(), -1);
    EXPECT_EQ(py::mod(py::int_(7), py::int_(2)).raw(), 1);
    EXPECT_EQ(py::mod(py::int_(-7), py::int_(-2)).raw(), -1);
}

TEST(RuntimeInt, PowWithNonNegativeExponent) {
    EXPECT_EQ(py::pow(py::int_(2), py::int_(10)).raw(), 1024);
    EXPECT_EQ(py::pow(py::int_(2), py::int_(0)).raw(), 1);
    EXPECT_EQ(py::pow(py::int_(0), py::int_(0)).raw(), 1);
    EXPECT_EQ(py::pow(py::int_(-2), py::int_(3)).raw(), -8);
}

TEST(RuntimeInt, Truthiness) {
    EXPECT_TRUE(py::truthy(py::int_(1)));
    EXPECT_FALSE(py::truthy(py::int_(0)));
    EXPECT_TRUE(py::truthy(py::bool_(true)));
    EXPECT_FALSE(py::truthy(py::none));
}

// Python's bool is an int subtype: True + 1 is 2.
TEST(RuntimeInt, BoolWidensToInt) {
    EXPECT_EQ(py::to_int(py::bool_(true)).raw(), 1);
    EXPECT_EQ(py::to_int(py::bool_(false)).raw(), 0);
    EXPECT_EQ(py::add(py::to_int(py::bool_(true)), py::int_(1)).raw(), 2);
}

// TASK 10B, obligation 1: to_int(bool_) must still be recognisably the bool
// it widened -- an annotation constrains, it does not coerce -- so the
// widened value keeps the Bool tag and prints/repr's as True/False, not 1/0.
TEST(RuntimeInt, ToIntWidensBoolWithoutCoercingItToANumber) {
    const py::int_ widened_true = py::to_int(py::bool_(true));
    EXPECT_TRUE(widened_true.is_bool());
    EXPECT_EQ(widened_true.raw(), 1);

    const py::int_ widened_false = py::to_int(py::bool_(false));
    EXPECT_TRUE(widened_false.is_bool());
    EXPECT_EQ(widened_false.raw(), 0);

    // Ordinary int_ values are never tagged Bool.
    EXPECT_FALSE(py::int_(1).is_bool());
}

// TASK 10B, obligation 2: `bool + bool` is an int, `True + True == 2`, and
// the result must NOT itself be tagged Bool (arithmetic always loses the
// Bool tag in Python, matching `type(True + True) is int`).
TEST(RuntimeInt, BoolPlusBoolIsAnUntaggedInt) {
    const py::int_ sum = py::add(py::to_int(py::bool_(true)), py::to_int(py::bool_(true)));
    EXPECT_EQ(sum.raw(), 2);
    EXPECT_FALSE(sum.is_bool());
}

// Overflow and division by zero exit the process; EXPECT_EXIT is how
// GoogleTest observes that without the test binary dying.
TEST(RuntimeIntDeathTest, OverflowExitsNonZero) {
    EXPECT_EXIT(py::add(py::int_(INT64_MAX), py::int_(1)),
                ::testing::ExitedWithCode(1), "OverflowError");
}

TEST(RuntimeIntDeathTest, DivisionByZeroExitsNonZero) {
    EXPECT_EXIT(py::floordiv(py::int_(1), py::int_(0)),
                ::testing::ExitedWithCode(1), "ZeroDivisionError");
    EXPECT_EXIT(py::mod(py::int_(1), py::int_(0)),
                ::testing::ExitedWithCode(1), "ZeroDivisionError");
}

// INT64_MIN / -1 has no representable result and is UB if transcribed.
TEST(RuntimeIntDeathTest, MinDividedByNegativeOneOverflows) {
    EXPECT_EXIT(py::floordiv(py::int_(INT64_MIN), py::int_(-1)),
                ::testing::ExitedWithCode(1), "OverflowError");
}

// FINAL-REVIEW deferred minor 4: the MODULO half of that same pair is the
// non-trapping branch -- the mathematical answer (0) IS representable, and
// mod()'s guard exists purely because the C++ `%` operator is UB for this
// pair. Its sibling floordiv has a death test; this branch, which is the one
// that actually returns a value, had nothing. Measured: CPython's
// `(-2**63) % -1` is 0.
TEST(RuntimeInt, MinModuloNegativeOneIsZeroAndDoesNotTrap) {
    EXPECT_EQ(py::mod(py::int_(INT64_MIN), py::int_(-1)).raw(), 0);
}

} // namespace
