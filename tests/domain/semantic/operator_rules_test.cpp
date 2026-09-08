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

void expect_unsupported_binary(token_type op, const Type& left, const Type& right,
                               UnsupportedReason reason) {
    const RuleResult result = binary_result(op, left, right);
    ASSERT_EQ(result.status, RuleResult::Status::Unsupported)
        << type_name(left) << " op " << type_name(right)
        << " should be a modelling limit, not a type error";
    EXPECT_EQ(result.reason, reason);
}

void expect_unsupported_comparison(token_type op, const Type& left, const Type& right,
                                   UnsupportedReason reason) {
    const RuleResult result = comparison_result(op, left, right);
    ASSERT_EQ(result.status, RuleResult::Status::Unsupported);
    EXPECT_EQ(result.reason, reason);
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
}

// Verified: reveal_type(b + ba) is bytes and reveal_type(ba + b) is
// bytearray -- concatenation is LEFT-TYPED and asymmetric. plus_result
// required an exact kind match, so both were false TypeErrors.
//
// CORRECTS A BACKWARDS COMMENT: the old note on
// ByteArrayConcatenatesRepeatsAndFormats called this "a missed error rather
// than a false one -- acceptable under invariant (a)". That is inverted.
// nullopt means the CALLER reports, so a caller makes this a FALSE TypeError
// on a program mypy accepts.
TEST(BinaryResult, MixedBytesAndByteArrayConcatenateToTheLeftOperandsType) {
    expect_binary(token_type::OP_PLUS, Type::bytes(), Type::bytearray_(), Type::bytes());
    expect_binary(token_type::OP_PLUS, Type::bytearray_(), Type::bytes(), Type::bytearray_());
}

// Verified: reveal_type([1] + [2]) is list[int], reveal_type([1] * 2) is
// list[int], and reveal_type([1] * True) is list[int].
TEST(BinaryResult, ListsConcatenateAndRepeat) {
    const Type ints = Type::list_of(Type::int_());

    expect_binary(token_type::OP_PLUS, ints, ints, ints);
    expect_binary(token_type::OP_STAR, ints, Type::int_(), ints);
    expect_binary(token_type::OP_STAR, Type::int_(), ints, ints);
    expect_binary(token_type::OP_STAR, ints, Type::bool_(), ints);

    expect_no_binary(token_type::OP_MINUS, ints, ints);
}

// Verified: reveal_type([1] + ["s"]) is list[str | int] -- mypy unions ANY
// element types, with no equivalence requirement. plus_result required
// is_equivalent, so list[int] + list[str] was a false TypeError.
//
// This looks wrong next to list invariance and is not: invariance governs
// ASSIGNING one list to another, while `+` builds a NEW list, so there is no
// aliasing hazard. mypy's union member order is right-then-left; ours is
// construction order, and is_equivalent makes the difference unobservable.
TEST(BinaryResult, ListsConcatenateAcrossDifferentElementTypes) {
    expect_binary(token_type::OP_PLUS, Type::list_of(Type::int_()),
                  Type::list_of(Type::str()),
                  Type::list_of(Type::union_of({Type::int_(), Type::str()})));
    expect_binary(token_type::OP_PLUS, Type::list_of(Type::int_()),
                  Type::list_of(Type::float_()),
                  Type::list_of(Type::union_of({Type::int_(), Type::float_()})));

    // Equivalent elements still collapse, because union_of de-duplicates.
    expect_binary(token_type::OP_PLUS, Type::list_of(Type::int_()),
                  Type::list_of(Type::int_()), Type::list_of(Type::int_()));
}

// Concatenation unions ANY element types (see ListsConcatenateAcrossDifferentElementTypes),
// so this test's remaining purpose is narrower than its name once implied: it
// pins that union_of's flattening makes the differently-ordered union
// spellings list[int | str] and list[str | int] concatenate to the same
// result regardless of which side is which.
TEST(BinaryResult, ListsConcatenateAcrossUnionMemberOrder) {
    const Type list_int_str = Type::list_of(Type::union_of({Type::int_(), Type::str()}));
    const Type list_str_int = Type::list_of(Type::union_of({Type::str(), Type::int_()}));

    expect_binary(token_type::OP_PLUS, list_int_str, list_str_int, list_int_str);
    expect_binary(token_type::OP_PLUS, list_str_int, list_int_str, list_str_int);
}

// Verified: reveal_type((1,) + (2,)) is tuple[int, int].
TEST(BinaryResult, TuplesConcatenateBySummingTheirArity) {
    expect_binary(token_type::OP_PLUS, Type::tuple_of({Type::int_()}),
                  Type::tuple_of({Type::str()}), Type::tuple_of({Type::int_(), Type::str()}));
    expect_binary(token_type::OP_PLUS, Type::tuple_of({}), Type::tuple_of({Type::int_()}),
                  Type::tuple_of({Type::int_()}));
}

// The three Unsupported arms, each a program mypy accepts.
//
// Verified: reveal_type(t * 2) on a tuple[int, str] is
// tuple[int, str, int, str] -- mypy unrolls the LITERAL count, so the result
// depends on an operand's value. Constant folding, which Spec 2 ruled out.
TEST(BinaryResult, RepeatingATupleIsUnsupportedNotAnError) {
    expect_unsupported_binary(token_type::OP_STAR, Type::tuple_of({Type::int_(), Type::str()}),
                              Type::int_(), UnsupportedReason::TupleRepeat);
    expect_unsupported_binary(token_type::OP_STAR, Type::int_(),
                              Type::tuple_of({Type::int_()}), UnsupportedReason::TupleRepeat);
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

// Verified against real mypy 1.18.1: reveal_type(d1 | d2) on two
// dict[str, int] is dict[str, int]; on dict[str, int] | dict[str, str] it is
// dict[str, int | str]; and on dict[str, int] | dict[int, int] it is
// dict[str | int, int] -- BOTH key and value types union, keys do NOT need
// to match. PEP 584, Python 3.9+. intersection_or_union_result had no Dict
// arm at all, so the first two rows were a false TypeError on a mypy-clean
// program; the key-matching requirement an earlier draft added for the third
// row was itself a false TypeError, extrapolated from a probe that only
// ever tried matching keys.
TEST(BinaryResult, DictsUnionTheirKeysAndValues) {
    const Type str_int = Type::dict_of(Type::str(), Type::int_());
    const Type str_str = Type::dict_of(Type::str(), Type::str());
    const Type int_int = Type::dict_of(Type::int_(), Type::int_());

    expect_binary(token_type::OP_PIPE, str_int, str_int, str_int);
    expect_binary(token_type::OP_PIPE, str_int, str_str,
                  Type::dict_of(Type::str(), Type::union_of({Type::int_(), Type::str()})));
    // Mismatched keys union too, rather than being an error.
    expect_binary(token_type::OP_PIPE, str_int, int_int,
                  Type::dict_of(Type::union_of({Type::str(), Type::int_()}), Type::int_()));
}

// Verified: dict & dict, dict - dict and dict ^ dict are all genuine
// [operator] errors ("Unsupported left operand type for &"). Pinned so the
// `|` arm above cannot be written as "any dict operator".
TEST(BinaryResult, DictsSupportNoOtherSetOperator) {
    const Type str_int = Type::dict_of(Type::str(), Type::int_());

    expect_no_binary(token_type::OP_AMPERSAND, str_int, str_int);
    expect_no_binary(token_type::OP_CARET, str_int, str_int);
    expect_no_binary(token_type::OP_MINUS, str_int, str_int);
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

TEST(BinaryResult, NoneSupportsNoArithmetic) {
    expect_no_binary(token_type::OP_PLUS, Type::none(), Type::none());
    expect_no_binary(token_type::OP_PLUS, Type::none(), Type::int_());
    expect_no_binary(token_type::OP_PLUS, Type::object(), Type::object());
}

// Verified: `v + 1` where V defines __add__(self, other: int) -> int is
// clean, result int. So a TypeError here would be false. `w + 1` on a class
// with no __add__ IS a genuine error, but distinguishing them needs dunder
// dispatch, which is deferred.
TEST(BinaryResult, OperatorsOnUserClassesAreUnsupportedNotErrors) {
    expect_unsupported_binary(token_type::OP_PLUS, Type::class_of("Widget"), Type::int_(),
                              UnsupportedReason::UserClassOperator);
    expect_unsupported_binary(token_type::OP_PLUS, Type::int_(), Type::class_of("Widget"),
                              UnsupportedReason::UserClassOperator);
    expect_unsupported_binary(token_type::OP_PLUS, Type::class_of("A"), Type::class_of("B"),
                              UnsupportedReason::UserClassOperator);
}

// The narrowing deferral, which already existed but travelled through
// nullopt and relied on every caller remembering to check for a Union first.
TEST(BinaryResult, UnionOperandsAreUnsupportedNotErrors) {
    const Type optional_int = Type::union_of({Type::int_(), Type::none()});

    expect_unsupported_binary(token_type::OP_PLUS, optional_int, Type::int_(),
                              UnsupportedReason::UnionOperand);
    expect_unsupported_binary(token_type::OP_PLUS, Type::int_(), optional_int,
                              UnsupportedReason::UnionOperand);
    expect_unsupported_binary(token_type::OP_PLUS, optional_int, optional_int,
                              UnsupportedReason::UnionOperand);
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
}

// Unary on a user class: verified `-w` with no __neg__ is an error but with
// __neg__ is clean. Unsupported. `not w` is clean on ANY type and stays Ok.
TEST(UnaryResult, ArithmeticOnUserClassesIsUnsupportedButNotIsNot) {
    const RuleResult negated = unary_result(token_type::OP_MINUS, Type::class_of("Widget"));
    ASSERT_EQ(negated.status, RuleResult::Status::Unsupported);
    EXPECT_EQ(negated.reason, UnsupportedReason::UserClassOperator);

    expect_unary(token_type::OP_NOT, Type::class_of("Widget"), Type::bool_());
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

// Verified: set[int] <= set[int], <, >, >= are all clean and yield bool; so
// is set[int] <= set[str] with DISJOINT elements; so is
// frozenset[int] <= set[int] in both directions.
//
// CORRECTS A BACKWARDS COMMENT: ordered_result's note called this a "cannot
// model" gap left nullopt "because this compiler's table has no way to
// distinguish 'cannot model' from 'type error'". Both halves are wrong. The
// answer is plainly bool, so it IS modellable -- it was a missing rule row,
// not a modelling limit.
TEST(ComparisonResult, SetsAndFrozenSetsSupportSubsetComparison) {
    const Type set_int = Type::set_of(Type::int_());
    const Type set_str = Type::set_of(Type::str());
    const Type frozen_int = Type::frozenset_of(Type::int_());

    for (const token_type op : {token_type::OP_LESS, token_type::OP_LESS_EQUAL,
                                token_type::OP_GREATER, token_type::OP_GREATER_EQUAL}) {
        expect_comparison(op, set_int, set_int);
        // Element types are NOT checked -- verified clean even when disjoint.
        expect_comparison(op, set_int, set_str);
        expect_comparison(op, frozen_int, frozen_int);
        expect_comparison(op, frozen_int, set_int);
        expect_comparison(op, set_int, frozen_int);
    }
}

// Verified: reveal_type(b < ba) is bool. ordered_result required matching
// kinds, so this was a false TypeError.
TEST(ComparisonResult, MixedBytesAndByteArrayCompare) {
    for (const token_type op : {token_type::OP_LESS, token_type::OP_LESS_EQUAL,
                                token_type::OP_GREATER, token_type::OP_GREATER_EQUAL}) {
        expect_comparison(op, Type::bytes(), Type::bytearray_());
        expect_comparison(op, Type::bytearray_(), Type::bytes());
    }
}

// Ordered comparison of a user class: verified `w < 1` with no __lt__ is a
// genuine error, but `l < 1` with __lt__ defined is clean. Unsupported.
// EQUALITY is different and stays Ok: == and is accept ANY operands and
// always yield bool, dunders or not.
TEST(ComparisonResult, OrderedComparisonOfUserClassesIsUnsupportedButEqualityIsNot) {
    expect_unsupported_comparison(token_type::OP_LESS, Type::class_of("Widget"),
                                  Type::int_(), UnsupportedReason::UserClassOperator);
    expect_comparison(token_type::OP_EQUAL, Type::class_of("Widget"), Type::int_());
    expect_comparison(token_type::OP_IS, Type::class_of("Widget"), Type::none());
}

// Unknown must absorb BEFORE the Unsupported gates in the ordered-comparison
// case: it is the absorbing bottom for "a root cause already reported", so
// treating it as a modelling limit would make the caller emit a SECOND,
// spurious NotImplementedError for an expression that already produced a
// diagnostic. Covers a Class and a Union on both sides, not just plain
// types, since the gates this must outrank are keyed on exactly those kinds.
TEST(ComparisonResult, AnOrderedComparisonWithUnknownIsBoolNotAnError) {
    expect_comparison(token_type::OP_LESS, Type::unknown(), Type::str());
    expect_comparison(token_type::OP_LESS, Type::int_(), Type::unknown());

    expect_comparison(token_type::OP_LESS, Type::unknown(), Type::class_of("Widget"));
    expect_comparison(token_type::OP_LESS, Type::class_of("Widget"), Type::unknown());

    const Type optional_int = Type::union_of({Type::int_(), Type::none()});
    expect_comparison(token_type::OP_LESS, Type::unknown(), optional_int);
    expect_comparison(token_type::OP_LESS, optional_int, Type::unknown());
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

        // The LEFT operand being a Class must stay clean: verified
        // `w in xs` where xs: list[W] is mypy-clean. Only the right operand
        // is gated -- see OperandOnTheRightBeingAUserClassOrUnionIsUnsupported.
        expect_comparison(op, Type::class_of("Widget"), Type::list_of(Type::class_of("Widget")));

        expect_no_comparison(op, Type::int_(), Type::int_());
        expect_no_comparison(op, Type::int_(), Type::none());
    }
}

// A seventh row: `in`/`not in` with a user class on the RIGHT. Verified
// against real mypy 1.18.1: `1 in w` where W defines
// __contains__(self, item: int) -> bool is clean (bool); `1 in p` on a class
// with no __contains__ is a genuine "Unsupported right operand type for in"
// [operator] error. Since the answer depends on the class's members, exactly
// like the dunder-dispatch deferral elsewhere, this must be Unsupported, not
// NotApplicable -- is_container excludes Class, so before this fix the table
// answered NotApplicable and a caller would emit a false TypeError on a
// mypy-clean program.
TEST(ComparisonResult, OperandOnTheRightBeingAUserClassOrUnionIsUnsupported) {
    for (const token_type op : {token_type::OP_IN, token_type::OP_NOT_IN}) {
        expect_unsupported_comparison(op, Type::int_(), Type::class_of("Widget"),
                                      UnsupportedReason::UserClassOperator);
        expect_unsupported_comparison(op, Type::int_(),
                                      Type::union_of({Type::list_of(Type::int_()), Type::none()}),
                                      UnsupportedReason::UnionOperand);
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
}

// Subscripting a user class: verified `s[0]` with
// __getitem__(self, i: int) -> str is clean. Unsupported, not an error.
TEST(SubscriptResult, SubscriptingAUserClassIsUnsupportedNotAnError) {
    const RuleResult result = subscript_result(Type::class_of("Widget"), Type::int_());
    ASSERT_EQ(result.status, RuleResult::Status::Unsupported);
    EXPECT_EQ(result.reason, UnsupportedReason::UserClassOperator);
}

TEST(SubscriptResult, SubscriptingAUnionIsUnsupported) {
    const RuleResult result =
        subscript_result(Type::union_of({Type::list_of(Type::int_()), Type::none()}), Type::int_());
    ASSERT_EQ(result.status, RuleResult::Status::Unsupported);
    EXPECT_EQ(result.reason, UnsupportedReason::UnionOperand);
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
    expect_not_iterable(Type::object());
}

// REWRITTEN. This test used to be named IteratingAUserClassStaysAGenuineError
// and pinned expect_not_iterable(Class("Widget")) -- a TypeError -- on the
// spec's claim that no import-free user class can be made iterable to mypy,
// "because __iter__ must return something with __next__, which cannot be
// spelled without typing/collections.abc". That premise is FALSE, and the
// test pinning it is why a false TypeError survived a green suite: mypy
// matches the iterator protocol STRUCTURALLY, so
//
//   class Counter:
//       def __next__(self) -> int: ...
//   class Bag:
//       def __iter__(self) -> Counter: ...
//   for v in Bag(): ...
//
// is mypy 1.18.1 --strict CLEAN with no import at all. Whether a user class
// is iterable therefore depends on members this rule table cannot see, which
// makes it Unsupported -- the same answer subscript_result and
// comparison_result already give a Class operand -- never NotApplicable.
// The cost is a MISSED error on a genuinely non-iterable class; the hard
// invariant (never a false TypeError) is what that buys.
TEST(ElementType, IteratingAUserClassIsUnsupportedNotAnError) {
    const RuleResult without_lookup = element_type(Type::class_of("Widget"));
    EXPECT_EQ(without_lookup.status, RuleResult::Status::Unsupported);
    EXPECT_EQ(without_lookup.reason, UnsupportedReason::UserClassIteration);

    // Same answer with a lookup that knows the class and its (empty) bases:
    // reaching no builtin is not evidence of non-iterability.
    const semantic_test_support::FakeClassLookup classes({{"Widget", {}}});
    const RuleResult with_lookup = element_type(Type::class_of("Widget"), &classes);
    EXPECT_EQ(with_lookup.status, RuleResult::Status::Unsupported);
    EXPECT_EQ(with_lookup.reason, UnsupportedReason::UserClassIteration);
}

// The precise half of the same rule: a class inheriting a builtin container
// iterates as that container's element type. Verified against mypy 1.18.1 --
// `class Names(str)`, `class Counts(bytes)` and `class Buf(bytearray)` are
// all clean, and iterating them reveals str, int and int respectively.
// (`range` is deliberately absent: typeshed marks it @final, so
// `class Steps(range)` is not a clean program to reason about at all.)
TEST(ElementType, IteratingAClassThatInheritsABuiltinYieldsTheBuiltinsElement) {
    const semantic_test_support::FakeClassLookup classes(
        {{"Names", {"str"}}, {"Counts", {"bytes"}}, {"Buf", {"bytearray"}}, {"Deep", {"Names"}}});
    const RuleResult names = element_type(Type::class_of("Names"), &classes);
    ASSERT_EQ(names.status, RuleResult::Status::Ok);
    EXPECT_EQ(names.type, Type::str());

    const RuleResult counts = element_type(Type::class_of("Counts"), &classes);
    ASSERT_EQ(counts.status, RuleResult::Status::Ok);
    EXPECT_EQ(counts.type, Type::int_());

    const RuleResult buf = element_type(Type::class_of("Buf"), &classes);
    ASSERT_EQ(buf.status, RuleResult::Status::Ok);
    EXPECT_EQ(buf.type, Type::int_());

    // Transitively, through the same cycle-guarded ancestor walk is_subtype
    // uses -- not just a direct base.
    const RuleResult deep = element_type(Type::class_of("Deep"), &classes);
    ASSERT_EQ(deep.status, RuleResult::Status::Ok);
    EXPECT_EQ(deep.type, Type::str());
}

// A NON-iterable builtin base does NOT make iteration an error: the class is
// free to define its own __iter__ on top of what it inherits, which this rule
// table cannot see. And a PARAMETRIC base (`class IntList(list[int])`) comes
// back argument-less, since ClassLookup deals in bare base NAMES, so there is
// no element type to report -- also Unsupported, never NotApplicable.
TEST(ElementType, AnInheritedBuiltinThatPinsNoElementTypeIsStillUnsupported) {
    const semantic_test_support::FakeClassLookup classes(
        {{"Sub", {"int"}}, {"IntList", {"list"}}});
    const RuleResult sub = element_type(Type::class_of("Sub"), &classes);
    EXPECT_EQ(sub.status, RuleResult::Status::Unsupported);
    EXPECT_EQ(sub.reason, UnsupportedReason::UserClassIteration);

    const RuleResult int_list = element_type(Type::class_of("IntList"), &classes);
    EXPECT_EQ(int_list.status, RuleResult::Status::Unsupported);
    EXPECT_EQ(int_list.reason, UnsupportedReason::UserClassIteration);
}

TEST(ElementType, IteratingAUnionIsUnsupported) {
    const RuleResult result = element_type(Type::union_of({Type::list_of(Type::int_()), Type::none()}));
    ASSERT_EQ(result.status, RuleResult::Status::Unsupported);
    EXPECT_EQ(result.reason, UnsupportedReason::UnionOperand);
}

// CORRECTED (Fix 4): this test used to pin element_type(tuple[()]) ==
// Unknown, on the reasoning that union_of({}) collapses there. But Unknown is
// the ABSORBING bottom for an error that was already reported, and nothing
// reports one for `tuple[()]` -- resolve_subscript accepts it silently. The
// old behaviour meant `for v in ()` bound v: Unknown and silenced every
// genuine error in the loop body. element_type now returns NotApplicable
// before consulting union_of, so the CALLER reports instead of silently
// absorbing.
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

void expect_boolop(token_type op, const std::vector<Type>& operands, const Type& expected) {
    const RuleResult result = boolop_result(op, operands);
    ASSERT_EQ(result.status, RuleResult::Status::Ok);
    EXPECT_EQ(result.type, expected) << "got " << type_name(result.type);
}

// Verified: reveal_type for `a and b` with both int is int; with bool and
// bool is bool. union_of de-duplicates, so same-typed operands collapse.
TEST(BoolOpResult, SameTypedOperandsCollapseToThatType) {
    expect_boolop(token_type::OP_AND, {Type::int_(), Type::int_()}, Type::int_());
    expect_boolop(token_type::OP_OR, {Type::bool_(), Type::bool_()}, Type::bool_());
    expect_boolop(token_type::OP_AND, {Type::str(), Type::str(), Type::str()}, Type::str());
}

// mypy gives `int and str` the type Literal[0] | str, applying falsiness
// narrowing to produce a literal out of a non-literal operand. int | str is a
// supertype of that, so every assignment mypy rejects we also reject; the gap
// is a MISSED error, never a false one.
TEST(BoolOpResult, MixedOperandsWidenToTheirUnion) {
    expect_boolop(token_type::OP_AND, {Type::int_(), Type::str()},
                  Type::union_of({Type::int_(), Type::str()}));
    expect_boolop(token_type::OP_OR, {Type::int_(), Type::str(), Type::none()},
                  Type::union_of({Type::int_(), Type::str(), Type::none()}));
}

// Verified: `a or b` with a: int | None and b: int reveals int -- mypy
// narrows None out of the left operand. We would produce int | None, and
// `y: int = a or b` would then be a FALSE TypeError. So the narrowing
// deferral has to cover BoolOp too.
TEST(BoolOpResult, UnionOperandsAreUnsupportedNotErrors) {
    const RuleResult result =
        boolop_result(token_type::OP_OR,
                      {Type::union_of({Type::int_(), Type::none()}), Type::int_()});

    ASSERT_EQ(result.status, RuleResult::Status::Unsupported);
    EXPECT_EQ(result.reason, UnsupportedReason::UnionOperand);
}

// Absorbing, like binary_result: one root cause, one diagnostic.
TEST(BoolOpResult, UnknownIsAbsorbing) {
    expect_boolop(token_type::OP_AND, {Type::unknown(), Type::int_()}, Type::unknown());
    expect_boolop(token_type::OP_OR, {Type::int_(), Type::unknown()}, Type::unknown());
}

// A user class is fine here: truthiness is universal, verified clean for
// `if w:` and for `w and 1` on a plain class. This is NOT a dunder question,
// so it must not be UserClassOperator.
TEST(BoolOpResult, UserClassOperandsAreFineBecauseTruthinessIsUniversal) {
    expect_boolop(token_type::OP_AND, {Type::class_of("Widget"), Type::class_of("Widget")},
                  Type::class_of("Widget"));
    expect_boolop(token_type::OP_OR, {Type::class_of("Widget"), Type::int_()},
                  Type::union_of({Type::class_of("Widget"), Type::int_()}));
}

TEST(BoolOpResult, IsNotApplicableForATokenThatIsNotABooleanOperator) {
    const RuleResult result = boolop_result(token_type::OP_PLUS, {Type::int_(), Type::int_()});
    EXPECT_EQ(result.status, RuleResult::Status::NotApplicable);
}

// ExpressionParser never builds a BoolOp with fewer than two values, but the
// function is total and must not index into an empty vector.
TEST(BoolOpResult, DegenerateOperandListsDoNotCrash) {
    EXPECT_EQ(boolop_result(token_type::OP_AND, {}).status, RuleResult::Status::NotApplicable);
    expect_boolop(token_type::OP_AND, {Type::int_()}, Type::int_());
}

} // namespace
} // namespace cythonpp::domain::semantic
