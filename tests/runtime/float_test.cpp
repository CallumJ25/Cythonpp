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

// FINAL-REVIEW CRITICAL 1. Every expected string below was produced by
// actually running CPython 3.14 (`repr(x)`), never by reasoning about what it
// ought to be. std::to_chars' format-less "general" form picks fixed vs
// scientific by SHORTEST STRING; CPython picks by DECIMAL EXPONENT
// (scientific iff decpt <= -4 || decpt > 16), and every row here is a value
// the two rules DISAGREE on -- each one printed the wrong text before the
// fix. ReprMatchesPython above stayed green throughout, which is exactly why
// these exist: its six values all happen to land where the two rules agree.
TEST(RuntimeFloat, ReprUsesCPythonsDecimalExponentRuleNotShortestString) {
    EXPECT_EQ(py::repr(py::float_(100000.0)), "100000.0");
    EXPECT_EQ(py::repr(py::float_(1000000.0)), "1000000.0");
    EXPECT_EQ(py::repr(py::float_(20000000.0)), "20000000.0");
    EXPECT_EQ(py::repr(py::float_(1000000000000000.0)), "1000000000000000.0");
    EXPECT_EQ(py::repr(py::float_(0.0001)), "0.0001");
    EXPECT_EQ(py::repr(py::float_(123456789012345678.0)), "1.2345678901234568e+17");
}

// BOTH SIDES of each of CPython's two thresholds. The upper one is decpt > 16,
// so 1e15 (decpt 16) is fixed and 1e16 (decpt 17) is scientific; the lower one
// is decpt <= -4, so 0.0001 (decpt -3) is fixed and 0.00001 (decpt -4) is
// scientific. A rule off by one in either direction fails one of each pair.
TEST(RuntimeFloat, ReprThresholdBoundariesMatchPythonOnBothSides) {
    EXPECT_EQ(py::repr(py::float_(1e15)), "1000000000000000.0");
    EXPECT_EQ(py::repr(py::float_(1e16)), "1e+16");
    EXPECT_EQ(py::repr(py::float_(1e17)), "1e+17");
    EXPECT_EQ(py::repr(py::float_(0.0001)), "0.0001");
    EXPECT_EQ(py::repr(py::float_(0.00001)), "1e-05");
    // The same two boundaries with a sign, and the largest fixed-form value
    // whose digit count exactly fills its decimal point position.
    EXPECT_EQ(py::repr(py::float_(-1e16)), "-1e+16");
    EXPECT_EQ(py::repr(py::float_(-0.00001)), "-1e-05");
    EXPECT_EQ(py::repr(py::float_(9999999999999998.0)), "9999999999999998.0");
    EXPECT_EQ(py::repr(py::float_(20000000000000008.0)), "2.000000000000001e+16");
}

// Shapes the fixed/scientific split must not disturb: a zero (and a NEGATIVE
// zero, which Python spells "-0.0"), an interior decimal point, the
// subnormal and finite extremes, and the two non-finite early returns.
TEST(RuntimeFloat, ReprHandlesZeroInteriorPointsAndExtremes) {
    EXPECT_EQ(py::repr(py::float_(0.0)), "0.0");
    EXPECT_EQ(py::repr(py::float_(-0.0)), "-0.0");
    EXPECT_EQ(py::repr(py::float_(12345.6789)), "12345.6789");
    EXPECT_EQ(py::repr(py::float_(0.001)), "0.001");
    EXPECT_EQ(py::repr(py::float_(1.5e-5)), "1.5e-05");
    EXPECT_EQ(py::repr(py::float_(5e-324)), "5e-324");
    EXPECT_EQ(py::repr(py::float_(1.7976931348623157e308)), "1.7976931348623157e+308");
    EXPECT_EQ(py::repr(py::float_(1e-300)), "1e-300");
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

// FINAL-REVIEW deferred minor 1: `//`, `%` and `**` on float_ are tag-aware
// (unlike `/`), and until now that had no coverage at all -- in the one
// subsystem where an unpinned change already shipped this effort's worst
// defect. Every expectation below was measured under CPython 3.14.
TEST(RuntimeFloat, FloorDivisionOnFloatIsTagAware) {
    const py::float_ seven = py::to_float(py::int_(7));
    const py::float_ two = py::to_float(py::int_(2));

    // 7 // 2 == 3, an INT, computed in int64_t rather than floored as a double.
    const py::float_ quotient = py::floordiv(seven, two);
    EXPECT_TRUE(quotient.is_integral());
    EXPECT_EQ(py::repr(quotient), "3");
    // Python floors toward negative infinity: -7 // 2 is -4, 7 // -2 is -4.
    EXPECT_EQ(py::repr(py::floordiv(py::to_float(py::int_(-7)), two)), "-4");
    EXPECT_EQ(py::repr(py::floordiv(seven, py::to_float(py::int_(-2)))), "-4");
    // A Bool-tagged operand is an int for arithmetic: True // 2 is 0.
    EXPECT_EQ(py::repr(py::floordiv(py::to_float(py::bool_(true)), two)), "0");
    // Mixing in a GENUINE float makes the result a genuine float: 7 // 2.0
    // is 3.0, not 3. Both operand orders.
    EXPECT_FALSE(py::floordiv(seven, py::float_(2.0)).is_integral());
    EXPECT_EQ(py::repr(py::floordiv(seven, py::float_(2.0))), "3.0");
    EXPECT_EQ(py::repr(py::floordiv(py::float_(7.0), py::int_(2))), "3.0");
    EXPECT_EQ(py::repr(py::floordiv(py::int_(7), py::float_(2.0))), "3.0");
}

TEST(RuntimeFloat, ModuloOnFloatIsTagAware) {
    const py::float_ seven = py::to_float(py::int_(7));
    const py::float_ two = py::to_float(py::int_(2));

    const py::float_ remainder = py::mod(seven, two);
    EXPECT_TRUE(remainder.is_integral());
    EXPECT_EQ(py::repr(remainder), "1");
    // Python's modulo takes the DIVISOR's sign: -7 % 2 is 1, 7 % -2 is -1.
    EXPECT_EQ(py::repr(py::mod(py::to_float(py::int_(-7)), two)), "1");
    EXPECT_EQ(py::repr(py::mod(seven, py::to_float(py::int_(-2)))), "-1");
    EXPECT_EQ(py::repr(py::mod(py::to_float(py::bool_(true)), two)), "1");
    EXPECT_EQ(py::repr(py::mod(seven, py::float_(2.0))), "1.0");
    EXPECT_EQ(py::repr(py::mod(py::float_(7.0), py::int_(2))), "1.0");
    EXPECT_EQ(py::repr(py::mod(py::int_(7), py::float_(2.0))), "1.0");
}

// FINAL-REVIEW "also fix": a ZERO remainder takes the divisor's sign too.
// The old `remainder != 0.0` guard skipped the correction at zero, so
// `7 % -0.5` was 0.0 where CPython says -0.0, and `-2.5 % 2.5` was -0.0 where
// CPython says 0.0. Visible in output, since repr spells the sign.
TEST(RuntimeFloat, ModuloKeepsTheDivisorsSignAtZero) {
    EXPECT_EQ(py::repr(py::mod(py::float_(7.0), py::float_(-0.5))), "-0.0");
    EXPECT_EQ(py::repr(py::mod(py::float_(-2.5), py::float_(2.5))), "0.0");
    EXPECT_EQ(py::repr(py::mod(py::float_(4.0), py::float_(-2.0))), "-0.0");
    EXPECT_EQ(py::repr(py::mod(py::float_(-4.0), py::float_(2.0))), "0.0");
}

TEST(RuntimeFloat, PowerOnFloatIsTagAware) {
    // 2 ** 10 held in float_-typed slots is the INT 1024, not 1024.0.
    const py::float_ power = py::pow(py::to_float(py::int_(2)), py::int_(10));
    EXPECT_TRUE(power.is_integral());
    EXPECT_EQ(py::repr(power), "1024");
    EXPECT_EQ(py::repr(py::pow(py::to_float(py::bool_(true)), py::int_(3))), "1");
    EXPECT_EQ(py::repr(py::pow(py::to_float(py::int_(-2)), py::int_(3))), "-8");
    // A genuine float base stays a genuine float.
    EXPECT_EQ(py::repr(py::pow(py::float_(2.5), py::int_(2))), "6.25");
    EXPECT_EQ(py::repr(py::pow(py::float_(2.0), py::int_(3))), "8.0");
}

// The zero-divisor trap has to fire through the TAG-AWARE path too (two
// integral float_s delegate to int_'s own floordiv/mod), not only through the
// double path FloatDivisionByZeroExitsNonZero already covers.
TEST(RuntimeFloatDeathTest, IntegralFloorDivisionAndModuloByZeroStillTrap) {
    const py::float_ seven = py::to_float(py::int_(7));
    const py::float_ zero = py::to_float(py::int_(0));
    EXPECT_EXIT(py::floordiv(seven, zero), ::testing::ExitedWithCode(1), "ZeroDivisionError");
    EXPECT_EXIT(py::mod(seven, zero), ::testing::ExitedWithCode(1), "ZeroDivisionError");
}

// FINAL-REVIEW deferred minor 2: truthy(float_) backs every `if x:`/`while x:`
// on a float-declared name, and RuntimeInt.Truthiness covers only int_. Both
// tag paths matter: an integral float_ tests int_raw(), a real one tests raw().
TEST(RuntimeFloat, Truthiness) {
    EXPECT_FALSE(py::truthy(py::float_(0.0)));
    EXPECT_FALSE(py::truthy(py::float_(-0.0)));
    EXPECT_TRUE(py::truthy(py::float_(0.5)));
    EXPECT_FALSE(py::truthy(py::to_float(py::int_(0))));
    EXPECT_TRUE(py::truthy(py::to_float(py::int_(-1))));
    EXPECT_FALSE(py::truthy(py::to_float(py::bool_(false))));
    EXPECT_TRUE(py::truthy(py::to_float(py::bool_(true))));
}

// FINAL-REVIEW deferred minor 2, the other half: `/` is deliberately NOT
// tag-aware -- Python's true division is always a float, even when both
// operands actually hold ints.
TEST(RuntimeFloat, TrueDivisionOnTwoIntegralFloatsIsStillAFloat) {
    const py::float_ result = py::truediv(py::to_float(py::int_(7)), py::to_float(py::int_(2)));
    EXPECT_FALSE(result.is_integral());
    EXPECT_EQ(py::repr(result), "3.5");
    // Exact division still yields a float, not an int: 4 / 2 is 2.0.
    EXPECT_EQ(py::repr(py::truediv(py::to_float(py::int_(4)), py::to_float(py::int_(2)))), "2.0");
}

// FINAL-REVIEW deferred minor 3: int_raw()/as_int() used to return the
// leftover int_value_ (always 0) when the tag says Float -- a silently wrong
// NUMBER, the one outcome this design forbids. They now fail loudly instead,
// through fail() rather than assert() so an NDEBUG build is covered too.
TEST(RuntimeFloatDeathTest, IntAccessorsOffPreconditionFailLoudly) {
    EXPECT_EXIT((void)py::float_(2.5).int_raw(), ::testing::ExitedWithCode(1), "SystemError");
    EXPECT_EXIT((void)py::float_(2.5).as_int(), ::testing::ExitedWithCode(1), "SystemError");
}

// C++ yields inf here rather than faulting; Python raises. This is the trap
// that makes float division-by-zero a check the runtime must ADD.
TEST(RuntimeFloatDeathTest, FloatDivisionByZeroExitsNonZero) {
    EXPECT_EXIT(py::truediv(py::float_(1.0), py::float_(0.0)),
                ::testing::ExitedWithCode(1), "ZeroDivisionError");
    EXPECT_EXIT(py::floordiv(py::float_(1.0), py::float_(0.0)),
                ::testing::ExitedWithCode(1), "ZeroDivisionError");
}

// The float_ arm of py::pos -- see int_test.cpp for why pos exists at all.
// The is_integral() branch is the load-bearing part: an integral value held in
// a float-declared slot must stay in int64_t rather than being routed through
// double, since a double is exact only to 2^53 while int_ reaches 2^63.
TEST(RuntimeFloat, PosPreservesIntegralityAndShedsTheBoolTag) {
    const py::float_ widened_bool = py::to_float(py::bool_(true));
    EXPECT_TRUE(widened_bool.is_integral());
    EXPECT_FALSE(py::pos(widened_bool).is_bool());
    EXPECT_TRUE(py::pos(widened_bool).is_integral());
    EXPECT_EQ(py::pos(widened_bool).int_raw(), 1);

    // A large integral value: exact here, corrupted if routed through double.
    const py::float_ large = py::to_float(py::int_(9007199254740993));
    EXPECT_TRUE(py::pos(large).is_integral());
    EXPECT_EQ(py::pos(large).int_raw(), 9007199254740993);

    // A genuine float keeps its value and stays non-integral.
    EXPECT_FALSE(py::pos(py::float_(2.5)).is_integral());
    EXPECT_EQ(py::pos(py::float_(2.5)).raw(), 2.5);
    EXPECT_EQ(py::pos(py::float_(-2.5)).raw(), -2.5);
}

} // namespace
