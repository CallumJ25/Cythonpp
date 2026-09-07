#include <gtest/gtest.h>

#include "domain/lexer/token_type.h"
#include "domain/semantic/operator_rules.h"
#include "domain/semantic/rule_result.h"
#include "domain/semantic/type.h"
#include "domain/semantic/type_name.h"
#include "fake_class_lookup.h"

namespace cythonpp::domain::semantic {
namespace {

using lexer::token_type;

// Reads as "left op right yields expected", and names both sides on failure,
// which is worth the wrapper when a table of rows fails at once.
void expect_binary(token_type op, const Type& left, const Type& right, const Type& expected) {
    const RuleResult result = binary_result(op, left, right);
    ASSERT_EQ(result.status, RuleResult::Status::Ok)
        << type_name(left) << " op " << type_name(right) << " should have a result type";
    EXPECT_EQ(result.type, expected) << "got " << type_name(result.type) << ", wanted "
                                     << type_name(expected);
}

// NotApplicable specifically, not merely "not Ok": Unsupported is also not
// Ok, and conflating them would let a modelling-limit arm silently satisfy a
// test that means "this is a genuine type error".
void expect_no_binary(token_type op, const Type& left, const Type& right) {
    const RuleResult result = binary_result(op, left, right);
    EXPECT_EQ(result.status, RuleResult::Status::NotApplicable)
        << type_name(left) << " op " << type_name(right) << " should be a type error, got "
        << (result.status == RuleResult::Status::Ok ? type_name(result.type) : "Unsupported");
}

void expect_unary(token_type op, const Type& operand, const Type& expected) {
    const RuleResult result = unary_result(op, operand);
    ASSERT_EQ(result.status, RuleResult::Status::Ok)
        << "unary op on " << type_name(operand) << " should have a result type";
    EXPECT_EQ(result.type, expected) << "got " << type_name(result.type);
}

void expect_no_unary(token_type op, const Type& operand) {
    const RuleResult result = unary_result(op, operand);
    EXPECT_EQ(result.status, RuleResult::Status::NotApplicable)
        << "unary op on " << type_name(operand) << " should be a type error";
}

void expect_comparison(token_type op, const Type& left, const Type& right) {
    const RuleResult result = comparison_result(op, left, right);
    ASSERT_EQ(result.status, RuleResult::Status::Ok)
        << type_name(left) << " cmp " << type_name(right) << " should have a result type";
    EXPECT_EQ(result.type, Type::bool_())
        << "a comparison must yield bool, got " << type_name(result.type);
}

void expect_no_comparison(token_type op, const Type& left, const Type& right) {
    const RuleResult result = comparison_result(op, left, right);
    EXPECT_EQ(result.status, RuleResult::Status::NotApplicable)
        << type_name(left) << " cmp " << type_name(right) << " should be a type error";
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

// mypy --strict verified: bytearray + bytearray, bytearray * int and
// bytearray % int are all clean (reveal_type: bytearray, bytearray, bytes
// respectively). Task 7 added ByteArray to is_container and ordered_result
// with a written argument for it; these three arms never got the same
// treatment, so bytearray was drawing a false "unsupported operand types".
TEST(BinaryResult, ByteArrayConcatenatesRepeatsAndFormats) {
    expect_binary(token_type::OP_PLUS, Type::bytearray_(), Type::bytearray_(), Type::bytearray_());
    expect_binary(token_type::OP_STAR, Type::bytearray_(), Type::int_(), Type::bytearray_());
    expect_binary(token_type::OP_STAR, Type::int_(), Type::bytearray_(), Type::bytearray_());

    // Verified: reveal_type(bytearray(b"x") % 3) is builtins.bytes, NOT
    // bytearray -- typeshed types bytearray.__mod__ to return bytes, which
    // disagrees with the real CPython runtime but is what mypy --strict
    // checks against. Modelling this as bytearray would make
    // `z: bytes = ba % 3` (mypy-clean) fail our own is_subtype check.
    expect_binary(token_type::OP_PERCENT, Type::bytearray_(), Type::int_(), Type::bytes());

    // Recorded gap, not fixed in this wave (out of the four instructed
    // arms): mypy --strict actually ACCEPTS mixed bytearray + bytes, typed as
    // bytearray (verified: reveal_type(bytearray(b"x") + b"y") is
    // builtins.bytearray). plus_result requires an exact kind match on both
    // sides, so this is a missed error rather than a false one -- acceptable
    // under invariant (a), but worth a follow-up arm in a future spec.
    expect_no_binary(token_type::OP_PLUS, Type::bytearray_(), Type::bytes());
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

// Elementwise EQUIVALENCE, not ==: list[int | str] and list[str | int] are
// the same type despite the differently-ordered union spelling, and mypy
// --strict accepts concatenating them. Genuinely different element types
// (int vs float) must still fail, in both directions.
TEST(BinaryResult, ListsConcatenateAcrossUnionMemberOrder) {
    const Type list_int_str = Type::list_of(Type::union_of({Type::int_(), Type::str()}));
    const Type list_str_int = Type::list_of(Type::union_of({Type::str(), Type::int_()}));

    expect_binary(token_type::OP_PLUS, list_int_str, list_str_int, list_int_str);
    expect_binary(token_type::OP_PLUS, list_str_int, list_int_str, list_str_int);

    expect_no_binary(token_type::OP_PLUS, Type::list_of(Type::int_()), Type::list_of(Type::float_()));
    expect_no_binary(token_type::OP_PLUS, Type::list_of(Type::float_()), Type::list_of(Type::int_()));
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

// Task 7 added ByteArray to is_container and ordered_result with a written
// argument for it; FrozenSet never got the same treatment for & and |, even
// though mypy --strict accepts both (verified: reveal_type(frozenset({1}) &
// frozenset({2})) and the | counterpart are both frozenset[int]).
TEST(BinaryResult, FrozenSetsIntersectAndUnion) {
    const Type ints = Type::frozenset_of(Type::int_());

    expect_binary(token_type::OP_AMPERSAND, ints, ints, ints);
    expect_binary(token_type::OP_PIPE, ints, ints, ints);
    expect_no_binary(token_type::OP_AMPERSAND, ints, Type::frozenset_of(Type::str()));
}

// Elementwise equivalence, not ==, matching the list-concatenation fix:
// set[int | str] and set[str | int] are the same type.
TEST(BinaryResult, SetsIntersectAcrossUnionMemberOrder) {
    const Type set_int_str = Type::set_of(Type::union_of({Type::int_(), Type::str()}));
    const Type set_str_int = Type::set_of(Type::union_of({Type::str(), Type::int_()}));

    expect_binary(token_type::OP_AMPERSAND, set_int_str, set_str_int, set_int_str);
    expect_binary(token_type::OP_PIPE, set_str_int, set_int_str, set_str_int);
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

TEST(UnaryResult, NegationAndPlusKeepTheNumericKind) {
    expect_unary(token_type::OP_MINUS, Type::int_(), Type::int_());
    expect_unary(token_type::OP_MINUS, Type::float_(), Type::float_());
    expect_unary(token_type::OP_MINUS, Type::complex_(), Type::complex_());
    expect_unary(token_type::OP_PLUS, Type::int_(), Type::int_());
    expect_unary(token_type::OP_PLUS, Type::float_(), Type::float_());
}

// Consistent with the binary rules: Bool widens to Int under arithmetic.
TEST(UnaryResult, NegatingABoolYieldsInt) {
    expect_unary(token_type::OP_MINUS, Type::bool_(), Type::int_());
    expect_unary(token_type::OP_PLUS, Type::bool_(), Type::int_());
}

TEST(UnaryResult, NegationDoesNotApplyToNonNumericTypes) {
    expect_no_unary(token_type::OP_MINUS, Type::str());
    expect_no_unary(token_type::OP_MINUS, Type::list_of(Type::int_()));
    expect_no_unary(token_type::OP_MINUS, Type::none());
    expect_no_unary(token_type::OP_MINUS, Type::class_of("Widget"));
}

TEST(UnaryResult, BitwiseNotIsIntegerOnly) {
    expect_unary(token_type::OP_TILDE, Type::int_(), Type::int_());
    expect_unary(token_type::OP_TILDE, Type::bool_(), Type::int_());
    expect_no_unary(token_type::OP_TILDE, Type::float_());
    expect_no_unary(token_type::OP_TILDE, Type::str());
}

// Truthiness is universal in Python: `if xs:` on a list is clean, verified.
TEST(UnaryResult, NotAcceptsAnyTypeAndAlwaysYieldsBool) {
    expect_unary(token_type::OP_NOT, Type::int_(), Type::bool_());
    expect_unary(token_type::OP_NOT, Type::str(), Type::bool_());
    expect_unary(token_type::OP_NOT, Type::list_of(Type::int_()), Type::bool_());
    expect_unary(token_type::OP_NOT, Type::none(), Type::bool_());
    expect_unary(token_type::OP_NOT, Type::class_of("Widget"), Type::bool_());
    expect_unary(token_type::OP_NOT, Type::union_of({Type::int_(), Type::none()}),
                 Type::bool_());
}

// The one place Unknown must NOT be absorbed: `not` always yields bool, and
// returning Unknown here would silence a genuine error downstream.
TEST(UnaryResult, NotOnUnknownIsStillBool) {
    expect_unary(token_type::OP_NOT, Type::unknown(), Type::bool_());
}

TEST(UnaryResult, UnknownIsAbsorbingForEveryOtherUnaryOperator) {
    expect_unary(token_type::OP_MINUS, Type::unknown(), Type::unknown());
    expect_unary(token_type::OP_PLUS, Type::unknown(), Type::unknown());
    expect_unary(token_type::OP_TILDE, Type::unknown(), Type::unknown());
}

TEST(UnaryResult, IsNulloptForATokenThatIsNotAUnaryOperator) {
    expect_no_unary(token_type::OP_STAR, Type::int_());
    expect_no_unary(token_type::OP_ASSIGN, Type::int_());
    expect_no_unary(token_type::OP_AND, Type::bool_());
}

// Verified: reveal_type(1 == "a") is bool, and the only complaint is
// comparison-overlap, which the spec puts out of scope.
TEST(ComparisonResult, EqualityAndIdentityAcceptAnyOperands) {
    for (const token_type op : {token_type::OP_EQUAL, token_type::OP_NOT_EQUAL,
                                token_type::OP_IS, token_type::OP_IS_NOT}) {
        expect_comparison(op, Type::int_(), Type::str());
        expect_comparison(op, Type::none(), Type::int_());
        expect_comparison(op, Type::list_of(Type::int_()), Type::class_of("Widget"));
        expect_comparison(op, Type::unknown(), Type::int_());
        expect_comparison(op, Type::union_of({Type::int_(), Type::none()}), Type::none());
    }
}

TEST(ComparisonResult, OrderedComparisonsRequireCompatibleOperands) {
    for (const token_type op : {token_type::OP_LESS, token_type::OP_LESS_EQUAL,
                                token_type::OP_GREATER, token_type::OP_GREATER_EQUAL}) {
        expect_comparison(op, Type::int_(), Type::int_());
        expect_comparison(op, Type::int_(), Type::float_());
        expect_comparison(op, Type::bool_(), Type::int_());
        expect_comparison(op, Type::str(), Type::str());
        expect_comparison(op, Type::bytes(), Type::bytes());
        expect_comparison(op, Type::list_of(Type::int_()), Type::list_of(Type::int_()));
        expect_comparison(op, Type::tuple_of({Type::int_()}), Type::tuple_of({Type::str()}));

        // Verified: 1 < "a" is "Unsupported operand types for <".
        expect_no_comparison(op, Type::int_(), Type::str());
        expect_no_comparison(op, Type::none(), Type::none());
        expect_no_comparison(op, Type::list_of(Type::int_()), Type::list_of(Type::str()));
        expect_no_comparison(op, Type::class_of("Widget"), Type::class_of("Widget"));
    }
}

// Elementwise equivalence, not ==, matching the list-concatenation and
// set-operation fixes: list[int | str] and list[str | int] are the same
// type, so an ordered comparison between them must still apply.
TEST(ComparisonResult, OrderedComparisonAppliesAcrossUnionMemberOrder) {
    const Type list_int_str = Type::list_of(Type::union_of({Type::int_(), Type::str()}));
    const Type list_str_int = Type::list_of(Type::union_of({Type::str(), Type::int_()}));

    for (const token_type op : {token_type::OP_LESS, token_type::OP_LESS_EQUAL,
                                token_type::OP_GREATER, token_type::OP_GREATER_EQUAL}) {
        expect_comparison(op, list_int_str, list_str_int);
        expect_comparison(op, list_str_int, list_int_str);
    }
}

TEST(ComparisonResult, AnOrderedComparisonWithUnknownIsBoolNotAnError) {
    expect_comparison(token_type::OP_LESS, Type::unknown(), Type::str());
    expect_comparison(token_type::OP_LESS, Type::int_(), Type::unknown());
}

TEST(ComparisonResult, MembershipRequiresAContainerOnTheRight) {
    for (const token_type op : {token_type::OP_IN, token_type::OP_NOT_IN}) {
        expect_comparison(op, Type::int_(), Type::list_of(Type::int_()));
        expect_comparison(op, Type::str(), Type::dict_of(Type::str(), Type::int_()));
        expect_comparison(op, Type::int_(), Type::set_of(Type::int_()));
        expect_comparison(op, Type::int_(), Type::frozenset_of(Type::int_()));
        expect_comparison(op, Type::int_(), Type::tuple_of({Type::int_()}));
        expect_comparison(op, Type::str(), Type::str());
        expect_comparison(op, Type::int_(), Type::bytes());
        expect_comparison(op, Type::int_(), Type::bytearray_());
        expect_comparison(op, Type::int_(), Type::range_());

        // Element compatibility is NOT checked: mypy's complaint about
        // `"a" in [1]` is comparison-overlap, which is out of scope.
        expect_comparison(op, Type::str(), Type::list_of(Type::int_()));

        expect_no_comparison(op, Type::int_(), Type::int_());
        expect_no_comparison(op, Type::int_(), Type::none());
        expect_no_comparison(op, Type::int_(), Type::class_of("Widget"));
    }
}

TEST(ComparisonResult, MembershipInUnknownIsBoolNotAnError) {
    expect_comparison(token_type::OP_IN, Type::int_(), Type::unknown());
}

TEST(ComparisonResult, IsNulloptForATokenThatIsNotAComparison) {
    expect_no_comparison(token_type::OP_PLUS, Type::int_(), Type::int_());
    expect_no_comparison(token_type::OP_AND, Type::bool_(), Type::bool_());
    expect_no_comparison(token_type::COLON, Type::int_(), Type::int_());
}

void expect_subscript(const Type& container, const Type& index, const Type& expected) {
    const RuleResult result = subscript_result(container, index);
    ASSERT_EQ(result.status, RuleResult::Status::Ok)
        << type_name(container) << "[" << type_name(index) << "] should have a result type";
    EXPECT_EQ(result.type, expected) << "got " << type_name(result.type);
}

void expect_no_subscript(const Type& container, const Type& index) {
    const RuleResult result = subscript_result(container, index);
    EXPECT_EQ(result.status, RuleResult::Status::NotApplicable)
        << type_name(container) << "[" << type_name(index) << "] should be a type error";
}

void expect_element(const Type& iterable, const Type& expected) {
    const RuleResult result = element_type(iterable);
    ASSERT_EQ(result.status, RuleResult::Status::Ok)
        << type_name(iterable) << " should be iterable";
    EXPECT_EQ(result.type, expected) << "got " << type_name(result.type);
}

void expect_not_iterable(const Type& iterable) {
    const RuleResult result = element_type(iterable);
    EXPECT_EQ(result.status, RuleResult::Status::NotApplicable)
        << type_name(iterable) << " should be a type error";
}

TEST(SubscriptResult, IndexingASequenceYieldsItsElement) {
    expect_subscript(Type::list_of(Type::str()), Type::int_(), Type::str());
    expect_subscript(Type::list_of(Type::str()), Type::bool_(), Type::str());
    expect_subscript(Type::str(), Type::int_(), Type::str());
    expect_subscript(Type::range_(), Type::int_(), Type::int_());
}

// Verified: reveal_type(bs[0]) on a bytes is int, not bytes.
TEST(SubscriptResult, IndexingBytesYieldsAnInt) {
    expect_subscript(Type::bytes(), Type::int_(), Type::int_());
    expect_subscript(Type::bytearray_(), Type::int_(), Type::int_());
}

TEST(SubscriptResult, ASequenceIndexMustBeAnInteger) {
    expect_no_subscript(Type::list_of(Type::str()), Type::str());
    expect_no_subscript(Type::str(), Type::str());
    expect_no_subscript(Type::bytes(), Type::float_());
    expect_no_subscript(Type::range_(), Type::none());
}

TEST(SubscriptResult, IndexingADictYieldsItsValueAndChecksTheKey) {
    const Type dict = Type::dict_of(Type::str(), Type::int_());

    expect_subscript(dict, Type::str(), Type::int_());
    // Verified: d[1] on a dict[str, int] is "Invalid index type".
    expect_no_subscript(dict, Type::int_());
    expect_no_subscript(dict, Type::none());
}

// The tower applies to the key check, because it goes through is_subtype.
TEST(SubscriptResult, ADictKeyIsCheckedByAssignabilityNotEquality) {
    expect_subscript(Type::dict_of(Type::float_(), Type::str()), Type::int_(), Type::str());
    expect_subscript(Type::dict_of(Type::object(), Type::str()), Type::none(), Type::str());
}

// The reason subscript_result takes a ClassLookup at all.
TEST(SubscriptResult, ADictKeyedByABaseClassAcceptsASubclassIndex) {
    const semantic_test_support::FakeClassLookup classes({{"Base", {}}, {"Sub", {"Base"}}});
    const Type dict = Type::dict_of(Type::class_of("Base"), Type::int_());

    EXPECT_EQ(subscript_result(dict, Type::class_of("Sub"), &classes).status,
              RuleResult::Status::Ok);
    // Without the lookup the two classes are unrelated, so it does not apply.
    EXPECT_EQ(subscript_result(dict, Type::class_of("Sub")).status,
              RuleResult::Status::NotApplicable);
}

// Verified: reveal_type(t[i]) on a tuple[int, str] with a variable index is
// int | str. mypy selects one member for a literal index, which needs
// literal types; the union is the sound approximation.
TEST(SubscriptResult, IndexingATupleYieldsTheUnionOfItsMembers) {
    expect_subscript(Type::tuple_of({Type::int_(), Type::str()}), Type::int_(),
                     Type::union_of({Type::int_(), Type::str()}));
    // A homogeneous tuple collapses, because union_of de-duplicates.
    expect_subscript(Type::tuple_of({Type::int_(), Type::int_()}), Type::int_(), Type::int_());
    expect_subscript(Type::tuple_of({Type::str()}), Type::int_(), Type::str());
}

// The subscript_result half of Fix 4: an empty tuple has no member to index,
// and nothing reported that, so this must not silently hand back Unknown
// either. No test at all existed for this before the fix.
TEST(SubscriptResult, IndexingAnEmptyTupleDoesNotApply) {
    expect_no_subscript(Type::tuple_of({}), Type::int_());
}

TEST(SubscriptResult, UnknownIsAbsorbingOnEitherSide) {
    expect_subscript(Type::unknown(), Type::int_(), Type::unknown());
    expect_subscript(Type::list_of(Type::str()), Type::unknown(), Type::unknown());
    expect_subscript(Type::none(), Type::unknown(), Type::unknown());
}

TEST(SubscriptResult, NonSubscriptableTypesDoNotApply) {
    expect_no_subscript(Type::int_(), Type::int_());
    expect_no_subscript(Type::none(), Type::int_());
    expect_no_subscript(Type::set_of(Type::int_()), Type::int_());
    expect_no_subscript(Type::class_of("Widget"), Type::int_());
    expect_no_subscript(Type::union_of({Type::list_of(Type::int_()), Type::none()}),
                        Type::int_());
}

TEST(ElementType, IteratingASequenceYieldsItsElement) {
    expect_element(Type::list_of(Type::str()), Type::str());
    expect_element(Type::set_of(Type::int_()), Type::int_());
    expect_element(Type::frozenset_of(Type::bool_()), Type::bool_());
    expect_element(Type::str(), Type::str());
    expect_element(Type::range_(), Type::int_());
}

// Verified: `for e in bs` on a bytes gives int.
TEST(ElementType, IteratingBytesYieldsAnInt) {
    expect_element(Type::bytes(), Type::int_());
    expect_element(Type::bytearray_(), Type::int_());
}

// Verified: `for k in d` on a dict[str, int] gives str, not a tuple.
TEST(ElementType, IteratingADictYieldsItsKeys) {
    expect_element(Type::dict_of(Type::str(), Type::int_()), Type::str());
}

// Verified: `for g in t` on a tuple[int, str] gives int | str, and on a
// tuple[int, int] gives int.
TEST(ElementType, IteratingATupleYieldsTheUnionOfItsMembers) {
    expect_element(Type::tuple_of({Type::int_(), Type::str()}),
                   Type::union_of({Type::int_(), Type::str()}));
    expect_element(Type::tuple_of({Type::int_(), Type::int_()}), Type::int_());
}

TEST(ElementType, UnknownIsAbsorbing) {
    expect_element(Type::unknown(), Type::unknown());
}

TEST(ElementType, NonIterableTypesDoNotApply) {
    expect_not_iterable(Type::int_());
    expect_not_iterable(Type::none());
    expect_not_iterable(Type::bool_());
    expect_not_iterable(Type::class_of("Widget"));
    expect_not_iterable(Type::object());
    expect_not_iterable(Type::union_of({Type::list_of(Type::int_()), Type::none()}));
}

// CORRECTED (Fix 4): this test used to pin element_type(tuple[()]) ==
// Unknown, on the reasoning that union_of({}) collapses there. But Unknown is
// the ABSORBING bottom for an error that was already reported, and nothing
// reports one for `tuple[()]` -- resolve_subscript accepts it silently. The
// old behaviour meant `for v in ()` bound v: Unknown and silenced every
// genuine error in the loop body. element_type now returns nullopt before
// consulting union_of, so the CALLER reports instead of silently absorbing.
TEST(ElementType, IteratingAnEmptyTupleDoesNotApply) {
    expect_not_iterable(Type::tuple_of({}));
}

// Unreachable through Type's factories, which always supply arguments, but
// these functions are total and must not index into an empty vector.
TEST(SubscriptAndElementType, AParameterlessContainerDoesNotApply) {
    Type bare;
    bare.kind = TypeKind::List;

    expect_no_subscript(bare, Type::int_());
    expect_not_iterable(bare);

    Type bare_dict;
    bare_dict.kind = TypeKind::Dict;

    expect_no_subscript(bare_dict, Type::str());
    expect_not_iterable(bare_dict);
}

} // namespace
} // namespace cythonpp::domain::semantic
