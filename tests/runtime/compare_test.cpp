#include <cmath>
#include <cstdint>

#include <gtest/gtest.h>

#include "cythonpp/compare.h"
#include "cythonpp/float_.h"
#include "cythonpp/int_.h"

namespace {

// TASK 10B, obligation 5: comparison compares VALUES across ranks.
TEST(RuntimeCompare, CrossRankEqualityMatchesPython) {
    EXPECT_TRUE(py::eq(py::int_(1), py::float_(1.0)).raw());
    EXPECT_TRUE(py::eq(py::float_(1.0), py::int_(1)).raw());
    EXPECT_TRUE(py::eq(py::to_int(py::bool_(true)), py::int_(1)).raw());
    EXPECT_FALSE(py::eq(py::int_(2), py::float_(1.0)).raw());
}

TEST(RuntimeCompare, CrossRankOrderingMatchesPython) {
    EXPECT_TRUE(py::lt(py::int_(1), py::float_(1.5)).raw());
    EXPECT_TRUE(py::gt(py::float_(1.5), py::int_(1)).raw());
    EXPECT_TRUE(py::le(py::int_(1), py::float_(1.0)).raw());
    EXPECT_TRUE(py::ge(py::float_(1.0), py::int_(1)).raw());
    EXPECT_TRUE(py::ne(py::int_(1), py::float_(1.5)).raw());
}

// The precision hazard obligation 5 calls out by name: naively converting a
// large int64_t to double before comparing can be wrong near the precision
// limit. 2**60 is exactly representable as a double (it is a power of two),
// but 2**60 + 1 is NOT (a double's 53-bit mantissa cannot hold the low bit at
// that magnitude) -- a naive `static_cast<double>(i) == d` comparison would
// therefore wrongly report 2**60 + 1 (int) as equal to 2.0**60 (double).
TEST(RuntimeCompare, LargeIntegerComparedAgainstDoubleIsExact) {
    const std::int64_t two_pow_60 = std::int64_t{1} << 60;
    ASSERT_EQ(static_cast<double>(two_pow_60 + 1), static_cast<double>(two_pow_60))
        << "test premise: this pair must actually collide when naively cast to double";

    EXPECT_TRUE(py::eq(py::int_(two_pow_60), py::float_(static_cast<double>(two_pow_60))).raw());
    EXPECT_FALSE(py::eq(py::int_(two_pow_60 + 1), py::float_(static_cast<double>(two_pow_60))).raw());
    EXPECT_TRUE(py::gt(py::int_(two_pow_60 + 1), py::float_(static_cast<double>(two_pow_60))).raw());
    EXPECT_TRUE(py::lt(py::float_(static_cast<double>(two_pow_60)), py::int_(two_pow_60 + 1)).raw());
}

// A float_ actually holding a large int (reached via to_float(int_), never
// through double) compares exactly against a plain int_ too -- the whole
// point of storing the int64_t rather than converting it at widening time.
TEST(RuntimeCompare, IntegralFloatComparedAgainstIntIsExact) {
    const std::int64_t big = 1000000000000000001LL; // 10**18 + 1, not exact as a double
    const py::float_ widened = py::to_float(py::int_(big));

    EXPECT_TRUE(py::eq(widened, py::int_(big)).raw());
    EXPECT_TRUE(py::eq(py::int_(big), widened).raw());
    EXPECT_FALSE(py::eq(widened, py::int_(big - 1)).raw());
    EXPECT_TRUE(py::gt(widened, py::int_(big - 1)).raw());
}

// Two integral float_ values compare exactly against EACH OTHER too, not
// just against a plain int_.
TEST(RuntimeCompare, TwoIntegralFloatsCompareExactly) {
    const std::int64_t big = 1000000000000000001LL;
    const py::float_ a = py::to_float(py::int_(big));
    const py::float_ b = py::to_float(py::int_(big));
    const py::float_ c = py::to_float(py::int_(big - 1));

    EXPECT_TRUE(py::eq(a, b).raw());
    EXPECT_FALSE(py::eq(a, c).raw());
    EXPECT_TRUE(py::gt(a, c).raw());
}

// NaN comparisons follow IEEE 754 / Python: every ordered comparison is
// False, and only `!=` is True, regardless of which side is the int.
TEST(RuntimeCompare, NanIsUnorderedAgainstAnInt) {
    const py::float_ nan = py::float_(std::nan(""));
    EXPECT_FALSE(py::eq(py::int_(1), nan).raw());
    EXPECT_FALSE(py::lt(py::int_(1), nan).raw());
    EXPECT_FALSE(py::gt(py::int_(1), nan).raw());
    EXPECT_TRUE(py::ne(py::int_(1), nan).raw());
    EXPECT_FALSE(py::eq(nan, py::int_(1)).raw());
    EXPECT_TRUE(py::ne(nan, py::int_(1)).raw());
}

} // namespace
