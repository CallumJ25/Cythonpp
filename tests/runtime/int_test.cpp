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

} // namespace
