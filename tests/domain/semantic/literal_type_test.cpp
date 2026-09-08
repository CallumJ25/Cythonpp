#include <string>

#include <gtest/gtest.h>

#include "domain/lexer/token_type.h"
#include "domain/semantic/literal_type.h"
#include "domain/semantic/type.h"

namespace cythonpp::domain::semantic {
namespace {

using lexer::token_type;

TEST(LiteralType, ClassifiesEveryLiteralAConstantCanCarry) {
    EXPECT_EQ(literal_type(token_type::LITERAL_INT), Type::int_());
    EXPECT_EQ(literal_type(token_type::LITERAL_FLOAT), Type::float_());
    EXPECT_EQ(literal_type(token_type::LITERAL_COMPLEX), Type::complex_());
    EXPECT_EQ(literal_type(token_type::LITERAL_STRING), Type::str());
    EXPECT_EQ(literal_type(token_type::LITERAL_BYTES), Type::bytes());
    EXPECT_EQ(literal_type(token_type::BOOL_TRUE), Type::bool_());
    EXPECT_EQ(literal_type(token_type::BOOL_FALSE), Type::bool_());
    EXPECT_EQ(literal_type(token_type::KEYWORD_NONE), Type::none());
    EXPECT_EQ(literal_type(token_type::ELLIPSIS), Type::ellipsis());
}

// LITERAL_INT and LITERAL_FLOAT are distinct even though AstPrinter erases
// the difference -- (Constant 1) prints identically for both.
TEST(LiteralType, DistinguishesIntFromFloat) {
    EXPECT_NE(literal_type(token_type::LITERAL_INT), literal_type(token_type::LITERAL_FLOAT));
}

TEST(LiteralType, IsUnknownForATokenThatIsNotALiteral) {
    EXPECT_EQ(literal_type(token_type::IDENTIFIER), Type::unknown());
    EXPECT_EQ(literal_type(token_type::OP_PLUS), Type::unknown());
    EXPECT_EQ(literal_type(token_type::KEYWORD_IF), Type::unknown());
    EXPECT_EQ(literal_type(token_type::TOKEN_ERROR), Type::unknown());
}

// allow_two_to_63 = true throughout this group: these pin the LOOSE bound
// (2^63 inclusive), the one ExpressionTyper applies only to the operand of a
// UnaryOp(-). The tightened (false) bound gets its own direct group below.
TEST(IntegerLiteralFits64Bits, AcceptsOrdinaryDecimalValues) {
    EXPECT_TRUE(integer_literal_fits_64_bits("0", true));
    EXPECT_TRUE(integer_literal_fits_64_bits("1", true));
    EXPECT_TRUE(integer_literal_fits_64_bits("42", true));
    EXPECT_TRUE(integer_literal_fits_64_bits("1000000", true));
}

TEST(IntegerLiteralFits64Bits, IgnoresUnderscoreSeparators) {
    EXPECT_TRUE(integer_literal_fits_64_bits("1_000", true));
    EXPECT_TRUE(integer_literal_fits_64_bits("1_000_000_000_000", true));
    EXPECT_TRUE(integer_literal_fits_64_bits("9_223_372_036_854_775_807", true));
    EXPECT_FALSE(integer_literal_fits_64_bits("9_223_372_036_854_775_809", true));
}

// 2^63 is ACCEPTED when allow_two_to_63 is true: this is a magnitude
// predicate, and -9223372036854775808 is a valid int64 whose minus sign is a
// UnaryOp the lexeme cannot see.
TEST(IntegerLiteralFits64Bits, AcceptsUpToTwoToTheSixtyThree) {
    EXPECT_TRUE(integer_literal_fits_64_bits("9223372036854775807", true));
    EXPECT_TRUE(integer_literal_fits_64_bits("9223372036854775808", true));
}

TEST(IntegerLiteralFits64Bits, RejectsBeyondTwoToTheSixtyThree) {
    EXPECT_FALSE(integer_literal_fits_64_bits("9223372036854775809", true));
    EXPECT_FALSE(integer_literal_fits_64_bits("18446744073709551615", true));
    EXPECT_FALSE(integer_literal_fits_64_bits("18446744073709551616", true));
    EXPECT_FALSE(integer_literal_fits_64_bits("1000000000000000000000000", true));
}

TEST(IntegerLiteralFits64Bits, ReadsTheHexadecimalPrefix) {
    EXPECT_TRUE(integer_literal_fits_64_bits("0xFF", true));
    EXPECT_TRUE(integer_literal_fits_64_bits("0xff", true));
    EXPECT_TRUE(integer_literal_fits_64_bits("0X10", true));
    EXPECT_TRUE(integer_literal_fits_64_bits("0x7FFFFFFFFFFFFFFF", true));
    EXPECT_TRUE(integer_literal_fits_64_bits("0x8000000000000000", true));
    EXPECT_FALSE(integer_literal_fits_64_bits("0x8000000000000001", true));
    EXPECT_FALSE(integer_literal_fits_64_bits("0xFFFFFFFFFFFFFFFF", true));
}

TEST(IntegerLiteralFits64Bits, ReadsTheOctalAndBinaryPrefixes) {
    EXPECT_TRUE(integer_literal_fits_64_bits("0o777", true));
    EXPECT_TRUE(integer_literal_fits_64_bits("0O10", true));
    EXPECT_TRUE(integer_literal_fits_64_bits("0o1000000000000000000000", true));
    EXPECT_FALSE(integer_literal_fits_64_bits("0o1000000000000000000001", true));

    EXPECT_TRUE(integer_literal_fits_64_bits("0b1010", true));
    EXPECT_TRUE(integer_literal_fits_64_bits("0B1", true));
    EXPECT_FALSE(integer_literal_fits_64_bits("0b" + std::string(65, '1'), true));
}

TEST(IntegerLiteralFits64Bits, AcceptsAPrefixedZero) {
    EXPECT_TRUE(integer_literal_fits_64_bits("0x0", true));
    EXPECT_TRUE(integer_literal_fits_64_bits("0b0", true));
    EXPECT_TRUE(integer_literal_fits_64_bits("0o0", true));
}

// Total: a lexeme carrying something this function does not recognise as a
// digit is not an integer literal to validate, and must not be reported.
TEST(IntegerLiteralFits64Bits, AcceptsALexemeItCannotRead) {
    EXPECT_TRUE(integer_literal_fits_64_bits("", true));
    EXPECT_TRUE(integer_literal_fits_64_bits("1e10", true));
}

// allow_two_to_63 = false: the bound ExpressionTyper applies to every
// Constant that is NOT the operand of a UnaryOp(-). Only the default (true)
// path was unit-tested directly before this; the interaction of the
// tightened bound with prefix parsing and underscore stripping was verified
// only end-to-end through ExpressionTyper. These pin it directly.
TEST(IntegerLiteralFits64Bits, TightensTheBoundByOneWhenNotAllowingTwoToTheSixtyThree) {
    EXPECT_FALSE(integer_literal_fits_64_bits("9223372036854775808", false))
        << "2^63 itself is one past int64_t's positive range";
    EXPECT_TRUE(integer_literal_fits_64_bits("9223372036854775808", true))
        << "the same lexeme is representable as -2^63 when the caller allows it";

    EXPECT_FALSE(integer_literal_fits_64_bits("0x8000000000000000", false))
        << "same boundary, hexadecimal spelling";
    EXPECT_TRUE(integer_literal_fits_64_bits("0x8000000000000000", true));

    EXPECT_FALSE(integer_literal_fits_64_bits("9_223_372_036_854_775_808", false))
        << "same boundary, with underscore separators";
    EXPECT_TRUE(integer_literal_fits_64_bits("9_223_372_036_854_775_808", true));

    EXPECT_TRUE(integer_literal_fits_64_bits("9223372036854775807", false))
        << "2^63 - 1 is representable regardless of which bound applies";
    EXPECT_TRUE(integer_literal_fits_64_bits("9223372036854775807", true));
}

} // namespace
} // namespace cythonpp::domain::semantic
