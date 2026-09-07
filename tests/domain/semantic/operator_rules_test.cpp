#include <optional>

#include <gtest/gtest.h>

#include "domain/lexer/token_type.h"
#include "domain/semantic/operator_rules.h"
#include "domain/semantic/type.h"
#include "domain/semantic/type_name.h"

namespace cythonpp::domain::semantic {
namespace {

using lexer::token_type;

// Reads as "left op right yields expected", and names both sides on failure,
// which is worth the wrapper when a table of rows fails at once.
void expect_binary(token_type op, const Type& left, const Type& right, const Type& expected) {
    const std::optional<Type> result = binary_result(op, left, right);
    ASSERT_TRUE(result.has_value()) << type_name(left) << " op " << type_name(right)
                                    << " should have a result type";
    EXPECT_EQ(*result, expected) << "got " << type_name(*result) << ", wanted "
                                 << type_name(expected);
}

void expect_no_binary(token_type op, const Type& left, const Type& right) {
    const std::optional<Type> result = binary_result(op, left, right);
    EXPECT_FALSE(result.has_value())
        << type_name(left) << " op " << type_name(right) << " should not apply, got "
        << (result ? type_name(*result) : "nullopt");
}

TEST(BinaryResult, UnknownIsAbsorbingAndNeverNullopt) {
    expect_binary(token_type::OP_PLUS, Type::unknown(), Type::int_(), Type::unknown());
    expect_binary(token_type::OP_PLUS, Type::int_(), Type::unknown(), Type::unknown());
    // Even for an operator that would otherwise not apply at all: the root
    // cause already reported, and a second diagnostic here would cascade.
    expect_binary(token_type::OP_AT, Type::unknown(), Type::unknown(), Type::unknown());
    expect_binary(token_type::OP_MINUS, Type::unknown(), Type::str(), Type::unknown());
}

TEST(BinaryResult, ArithmeticWidensAlongTheNumericTower) {
    expect_binary(token_type::OP_PLUS, Type::int_(), Type::int_(), Type::int_());
    expect_binary(token_type::OP_PLUS, Type::int_(), Type::float_(), Type::float_());
    expect_binary(token_type::OP_PLUS, Type::float_(), Type::int_(), Type::float_());
    expect_binary(token_type::OP_PLUS, Type::float_(), Type::complex_(), Type::complex_());
    expect_binary(token_type::OP_MINUS, Type::int_(), Type::float_(), Type::float_());
}

// Verified: reveal_type(True + True) is int, and reveal_type(True + 1) is int.
TEST(BinaryResult, BoolOperandsWidenToInt) {
    expect_binary(token_type::OP_PLUS, Type::bool_(), Type::bool_(), Type::int_());
    expect_binary(token_type::OP_PLUS, Type::bool_(), Type::int_(), Type::int_());
    expect_binary(token_type::OP_MINUS, Type::bool_(), Type::bool_(), Type::int_());
}

// The single most surprising row in the table. Python 3's `/` is always true
// division, so int / int is float.
TEST(BinaryResult, TrueDivisionAlwaysYieldsFloat) {
    expect_binary(token_type::OP_SLASH, Type::int_(), Type::int_(), Type::float_());
    expect_binary(token_type::OP_SLASH, Type::bool_(), Type::bool_(), Type::float_());
    expect_binary(token_type::OP_SLASH, Type::float_(), Type::int_(), Type::float_());
    expect_binary(token_type::OP_SLASH, Type::complex_(), Type::int_(), Type::complex_());
}

TEST(BinaryResult, FloorDivisionKeepsTheTowersJoin) {
    expect_binary(token_type::OP_DOUBLE_SLASH, Type::int_(), Type::int_(), Type::int_());
    expect_binary(token_type::OP_DOUBLE_SLASH, Type::float_(), Type::float_(), Type::float_());
}

// int ** int is int. mypy types 2 ** -1 as float by reading the literal's
// sign; matching that needs literal types. A recorded gap, not a bug.
TEST(BinaryResult, PowerKeepsTheTowersJoin) {
    expect_binary(token_type::OP_DOUBLE_STAR, Type::int_(), Type::int_(), Type::int_());
    expect_binary(token_type::OP_DOUBLE_STAR, Type::float_(), Type::int_(), Type::float_());
}

TEST(BinaryResult, StringsConcatenateAndRepeat) {
    expect_binary(token_type::OP_PLUS, Type::str(), Type::str(), Type::str());
    expect_binary(token_type::OP_STAR, Type::str(), Type::int_(), Type::str());
    expect_binary(token_type::OP_STAR, Type::int_(), Type::str(), Type::str());
    expect_binary(token_type::OP_STAR, Type::str(), Type::bool_(), Type::str());

    expect_no_binary(token_type::OP_PLUS, Type::str(), Type::int_());
    expect_no_binary(token_type::OP_MINUS, Type::str(), Type::str());
    expect_no_binary(token_type::OP_STAR, Type::str(), Type::str());
    expect_no_binary(token_type::OP_STAR, Type::str(), Type::float_());
}

TEST(BinaryResult, BytesConcatenateAndRepeat) {
    expect_binary(token_type::OP_PLUS, Type::bytes(), Type::bytes(), Type::bytes());
    expect_binary(token_type::OP_STAR, Type::bytes(), Type::int_(), Type::bytes());
    expect_no_binary(token_type::OP_PLUS, Type::bytes(), Type::str());
}

// Verified: reveal_type([1] + [2]) is list[int], reveal_type([1] * 2) is
// list[int], and reveal_type([1] * True) is list[int].
TEST(BinaryResult, ListsConcatenateWhenTheirElementsMatchExactly) {
    const Type ints = Type::list_of(Type::int_());
    const Type floats = Type::list_of(Type::float_());

    expect_binary(token_type::OP_PLUS, ints, ints, ints);
    expect_binary(token_type::OP_STAR, ints, Type::int_(), ints);
    expect_binary(token_type::OP_STAR, Type::int_(), ints, ints);
    expect_binary(token_type::OP_STAR, ints, Type::bool_(), ints);

    // Element types are compared exactly, matching list's invariance: there
    // is no join that would be sound for a mutable container.
    expect_no_binary(token_type::OP_PLUS, ints, floats);
    expect_no_binary(token_type::OP_MINUS, ints, ints);
}

// Verified: reveal_type((1,) + (2,)) is tuple[int, int].
TEST(BinaryResult, TuplesConcatenateBySummingTheirArity) {
    expect_binary(token_type::OP_PLUS, Type::tuple_of({Type::int_()}),
                  Type::tuple_of({Type::str()}), Type::tuple_of({Type::int_(), Type::str()}));
    expect_binary(token_type::OP_PLUS, Type::tuple_of({}), Type::tuple_of({Type::int_()}),
                  Type::tuple_of({Type::int_()}));
}

TEST(BinaryResult, ModuloIsArithmeticAndAlsoStringFormatting) {
    expect_binary(token_type::OP_PERCENT, Type::int_(), Type::int_(), Type::int_());
    expect_binary(token_type::OP_PERCENT, Type::float_(), Type::int_(), Type::float_());

    // printf-style. The placeholders are not checked against the right
    // operand: that is mypy's str-format rule, which the spec puts out of
    // scope, so anything is accepted on the right.
    expect_binary(token_type::OP_PERCENT, Type::str(), Type::int_(), Type::str());
    expect_binary(token_type::OP_PERCENT, Type::str(), Type::tuple_of({Type::int_()}),
                  Type::str());
    expect_binary(token_type::OP_PERCENT, Type::bytes(), Type::int_(), Type::bytes());

    expect_no_binary(token_type::OP_PERCENT, Type::int_(), Type::str());
    expect_no_binary(token_type::OP_PERCENT, Type::list_of(Type::int_()), Type::int_());
}

TEST(BinaryResult, BitwiseOperatorsAreIntegerOnly) {
    expect_binary(token_type::OP_AMPERSAND, Type::int_(), Type::int_(), Type::int_());
    expect_binary(token_type::OP_PIPE, Type::int_(), Type::int_(), Type::int_());
    expect_binary(token_type::OP_CARET, Type::int_(), Type::int_(), Type::int_());
    expect_binary(token_type::OP_LEFT_SHIFT, Type::int_(), Type::int_(), Type::int_());
    expect_binary(token_type::OP_RIGHT_SHIFT, Type::int_(), Type::int_(), Type::int_());
    expect_binary(token_type::OP_AMPERSAND, Type::bool_(), Type::bool_(), Type::int_());

    // Verified: 1 & 2.0 is an error -- "Unsupported operand types for &".
    expect_no_binary(token_type::OP_AMPERSAND, Type::int_(), Type::float_());
    expect_no_binary(token_type::OP_LEFT_SHIFT, Type::int_(), Type::float_());
    expect_no_binary(token_type::OP_CARET, Type::str(), Type::str());
}

// Verified: reveal_type({1} & {2}) and reveal_type({1} | {2}) are both
// set[int]. Only & and | are modelled; ^ on sets is a recorded gap.
TEST(BinaryResult, SetsIntersectAndUnion) {
    const Type ints = Type::set_of(Type::int_());

    expect_binary(token_type::OP_AMPERSAND, ints, ints, ints);
    expect_binary(token_type::OP_PIPE, ints, ints, ints);
    expect_no_binary(token_type::OP_AMPERSAND, ints, Type::set_of(Type::str()));
    expect_no_binary(token_type::OP_CARET, ints, ints);
}

// The semantic half of OP_AT's two readings. Spec 4 settled the syntactic
// half -- decorator at statement position, matrix-multiply elsewhere -- and
// no builtin type supports matrix multiplication, so this never applies.
TEST(BinaryResult, MatrixMultiplyNeverApplies) {
    expect_no_binary(token_type::OP_AT, Type::int_(), Type::int_());
    expect_no_binary(token_type::OP_AT, Type::list_of(Type::int_()), Type::list_of(Type::int_()));
    expect_no_binary(token_type::OP_AT, Type::object(), Type::object());
}

TEST(BinaryResult, IsNulloptForATokenThatIsNotABinaryOperator) {
    expect_no_binary(token_type::OP_ASSIGN, Type::int_(), Type::int_());
    expect_no_binary(token_type::OP_LESS, Type::int_(), Type::int_());
    expect_no_binary(token_type::OP_AND, Type::bool_(), Type::bool_());
    expect_no_binary(token_type::COMMA, Type::int_(), Type::int_());
}

TEST(BinaryResult, NoneAndClassesSupportNoArithmetic) {
    expect_no_binary(token_type::OP_PLUS, Type::none(), Type::none());
    expect_no_binary(token_type::OP_PLUS, Type::none(), Type::int_());
    expect_no_binary(token_type::OP_PLUS, Type::class_of("Widget"), Type::int_());
    expect_no_binary(token_type::OP_PLUS, Type::object(), Type::object());
}

// A union-typed operand does not get an arithmetic result: the spec defers
// narrowing, so the CALLER reports "operations on a union-typed value require
// narrowing, which is not supported". nullopt is what makes that possible.
TEST(BinaryResult, UnionOperandsDoNotApply) {
    const Type optional_int = Type::union_of({Type::int_(), Type::none()});

    expect_no_binary(token_type::OP_PLUS, optional_int, Type::int_());
    expect_no_binary(token_type::OP_PLUS, Type::int_(), optional_int);
    expect_no_binary(token_type::OP_PLUS, optional_int, optional_int);
}

} // namespace
} // namespace cythonpp::domain::semantic
