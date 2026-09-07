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

TEST(IntegerLiteralFits64Bits, AcceptsOrdinaryDecimalValues) {
    EXPECT_TRUE(integer_literal_fits_64_bits("0"));
    EXPECT_TRUE(integer_literal_fits_64_bits("1"));
    EXPECT_TRUE(integer_literal_fits_64_bits("42"));
    EXPECT_TRUE(integer_literal_fits_64_bits("1000000"));
}

TEST(IntegerLiteralFits64Bits, IgnoresUnderscoreSeparators) {
    EXPECT_TRUE(integer_literal_fits_64_bits("1_000"));
    EXPECT_TRUE(integer_literal_fits_64_bits("1_000_000_000_000"));
    EXPECT_TRUE(integer_literal_fits_64_bits("9_223_372_036_854_775_807"));
    EXPECT_FALSE(integer_literal_fits_64_bits("9_223_372_036_854_775_809"));
}

// 2^63 is ACCEPTED: this is a magnitude predicate, and
// -9223372036854775808 is a valid int64 whose minus sign is a UnaryOp the
// lexeme cannot see.
TEST(IntegerLiteralFits64Bits, AcceptsUpToTwoToTheSixtyThree) {
    EXPECT_TRUE(integer_literal_fits_64_bits("9223372036854775807"));
    EXPECT_TRUE(integer_literal_fits_64_bits("9223372036854775808"));
}

TEST(IntegerLiteralFits64Bits, RejectsBeyondTwoToTheSixtyThree) {
    EXPECT_FALSE(integer_literal_fits_64_bits("9223372036854775809"));
    EXPECT_FALSE(integer_literal_fits_64_bits("18446744073709551615"));
    EXPECT_FALSE(integer_literal_fits_64_bits("18446744073709551616"));
    EXPECT_FALSE(integer_literal_fits_64_bits("1000000000000000000000000"));
}

TEST(IntegerLiteralFits64Bits, ReadsTheHexadecimalPrefix) {
    EXPECT_TRUE(integer_literal_fits_64_bits("0xFF"));
    EXPECT_TRUE(integer_literal_fits_64_bits("0xff"));
    EXPECT_TRUE(integer_literal_fits_64_bits("0X10"));
    EXPECT_TRUE(integer_literal_fits_64_bits("0x7FFFFFFFFFFFFFFF"));
    EXPECT_TRUE(integer_literal_fits_64_bits("0x8000000000000000"));
    EXPECT_FALSE(integer_literal_fits_64_bits("0x8000000000000001"));
    EXPECT_FALSE(integer_literal_fits_64_bits("0xFFFFFFFFFFFFFFFF"));
}

TEST(IntegerLiteralFits64Bits, ReadsTheOctalAndBinaryPrefixes) {
    EXPECT_TRUE(integer_literal_fits_64_bits("0o777"));
    EXPECT_TRUE(integer_literal_fits_64_bits("0O10"));
    EXPECT_TRUE(integer_literal_fits_64_bits("0o1000000000000000000000"));
    EXPECT_FALSE(integer_literal_fits_64_bits("0o1000000000000000000001"));

    EXPECT_TRUE(integer_literal_fits_64_bits("0b1010"));
    EXPECT_TRUE(integer_literal_fits_64_bits("0B1"));
    EXPECT_FALSE(integer_literal_fits_64_bits("0b" + std::string(65, '1')));
}

TEST(IntegerLiteralFits64Bits, AcceptsAPrefixedZero) {
    EXPECT_TRUE(integer_literal_fits_64_bits("0x0"));
    EXPECT_TRUE(integer_literal_fits_64_bits("0b0"));
    EXPECT_TRUE(integer_literal_fits_64_bits("0o0"));
}

// Total: a lexeme carrying something this function does not recognise as a
// digit is not an integer literal to validate, and must not be reported.
TEST(IntegerLiteralFits64Bits, AcceptsALexemeItCannotRead) {
    EXPECT_TRUE(integer_literal_fits_64_bits(""));
    EXPECT_TRUE(integer_literal_fits_64_bits("1e10"));
}

} // namespace
} // namespace cythonpp::domain::semantic
