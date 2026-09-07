#include <gtest/gtest.h>

#include "domain/semantic/type.h"
#include "domain/semantic/type_compatibility.h"
#include "domain/semantic/type_name.h"

#include "fake_class_lookup.h"

namespace cythonpp::domain::semantic {
namespace {

// Reads as "source is assignable to target", and prints both names on
// failure, which is worth the wrapper when a table of rows fails.
void expect_subtype(const Type& source, const Type& target) {
    EXPECT_TRUE(is_subtype(source, target))
        << type_name(source) << " should be assignable to " << type_name(target);
}

void expect_not_subtype(const Type& source, const Type& target) {
    EXPECT_FALSE(is_subtype(source, target))
        << type_name(source) << " should NOT be assignable to " << type_name(target);
}

TEST(IsSubtype, IdenticalTypesAreAssignable) {
    expect_subtype(Type::int_(), Type::int_());
    expect_subtype(Type::str(), Type::str());
    expect_subtype(Type::list_of(Type::int_()), Type::list_of(Type::int_()));
    expect_subtype(Type::dict_of(Type::str(), Type::int_()),
                   Type::dict_of(Type::str(), Type::int_()));
}

TEST(IsSubtype, UnknownIsAbsorbingInBothDirections) {
    expect_subtype(Type::unknown(), Type::int_());
    expect_subtype(Type::int_(), Type::unknown());
    expect_subtype(Type::unknown(), Type::unknown());
    expect_subtype(Type::unknown(), Type::list_of(Type::str()));
    expect_subtype(Type::list_of(Type::str()), Type::unknown());
}

TEST(IsSubtype, ObjectIsTheTopType) {
    expect_subtype(Type::int_(), Type::object());
    expect_subtype(Type::none(), Type::object());
    expect_subtype(Type::list_of(Type::int_()), Type::object());
    expect_subtype(Type::range_(), Type::object());
    expect_subtype(Type::object(), Type::object());
}

TEST(IsSubtype, ObjectIsNotAssignableToAnythingNarrower) {
    expect_not_subtype(Type::object(), Type::int_());
    expect_not_subtype(Type::object(), Type::str());
}

TEST(IsSubtype, TheNumericTowerWidensButDoesNotNarrow) {
    expect_subtype(Type::bool_(), Type::int_());
    expect_subtype(Type::int_(), Type::float_());
    expect_subtype(Type::float_(), Type::complex_());

    expect_not_subtype(Type::int_(), Type::bool_());
    expect_not_subtype(Type::float_(), Type::int_());
    expect_not_subtype(Type::complex_(), Type::float_());
}

// Verified against mypy: `complex = True` and `float = True` are both clean.
TEST(IsSubtype, TheNumericTowerIsTransitive) {
    expect_subtype(Type::bool_(), Type::float_());
    expect_subtype(Type::bool_(), Type::complex_());
    expect_subtype(Type::int_(), Type::complex_());
}

TEST(IsSubtype, NonNumericTypesAreOutsideTheTower) {
    expect_not_subtype(Type::str(), Type::int_());
    expect_not_subtype(Type::int_(), Type::str());
    expect_not_subtype(Type::none(), Type::int_());
    expect_not_subtype(Type::bytes(), Type::str());
}

// mypy says outright: "list" is invariant. list[float] = list[int] would let
// a float be appended to a list of int through the alias.
TEST(IsSubtype, MutableContainersAreInvariant) {
    expect_not_subtype(Type::list_of(Type::int_()), Type::list_of(Type::float_()));
    expect_not_subtype(Type::list_of(Type::float_()), Type::list_of(Type::int_()));
    expect_not_subtype(Type::set_of(Type::int_()), Type::set_of(Type::float_()));
    expect_not_subtype(Type::frozenset_of(Type::int_()), Type::frozenset_of(Type::float_()));
    expect_not_subtype(Type::dict_of(Type::str(), Type::int_()),
                       Type::dict_of(Type::str(), Type::float_()));
    expect_not_subtype(Type::list_of(Type::int_()), Type::list_of(Type::object()));
}

// Verified against mypy: tuple[float, float] = tuple[int, int] is clean,
// because a tuple is immutable and so may be covariant.
// The Critical finding this wave exists to fix: a union nested INSIDE an
// invariant container used to have only source == target left as a success
// path, so list[int | str] and list[str | int] -- the same type, verified
// against mypy --strict as mutually assignable -- came back false. Elementwise
// is_equivalent (never ==) is what makes this order-insensitive at any depth,
// not just at the top level.
TEST(IsSubtype, InvariantContainersAreOrderInsensitiveForUnionElements) {
    const Type list_int_str = Type::list_of(Type::union_of({Type::int_(), Type::str()}));
    const Type list_str_int = Type::list_of(Type::union_of({Type::str(), Type::int_()}));
    expect_subtype(list_int_str, list_str_int);
    expect_subtype(list_str_int, list_int_str);

    const Type dict_int_none = Type::dict_of(Type::str(), Type::union_of({Type::int_(), Type::none()}));
    const Type dict_none_int = Type::dict_of(Type::str(), Type::union_of({Type::none(), Type::int_()}));
    expect_subtype(dict_int_none, dict_none_int);
    expect_subtype(dict_none_int, dict_int_none);

    // The fix must not have loosened invariance itself: a union spelling
    // difference is fine, but a genuinely different element type still is
    // not, in either direction.
    expect_not_subtype(Type::list_of(Type::int_()), Type::list_of(Type::float_()));
    expect_not_subtype(Type::list_of(Type::float_()), Type::list_of(Type::int_()));
}

TEST(IsSubtype, TuplesAreCovariantElementwise) {
    expect_subtype(Type::tuple_of({Type::int_(), Type::int_()}),
                   Type::tuple_of({Type::float_(), Type::float_()}));
    expect_subtype(Type::tuple_of({Type::bool_()}), Type::tuple_of({Type::object()}));
    expect_not_subtype(Type::tuple_of({Type::float_()}), Type::tuple_of({Type::int_()}));
}

TEST(IsSubtype, TupleArityMustMatch) {
    expect_not_subtype(Type::tuple_of({Type::int_()}),
                       Type::tuple_of({Type::int_(), Type::int_()}));
    expect_not_subtype(Type::tuple_of({Type::int_(), Type::int_()}),
                       Type::tuple_of({Type::int_()}));
    expect_subtype(Type::tuple_of({}), Type::tuple_of({}));
}

TEST(IsSubtype, DifferentKindsAreUnrelated) {
    expect_not_subtype(Type::list_of(Type::int_()), Type::set_of(Type::int_()));
    expect_not_subtype(Type::tuple_of({Type::int_()}), Type::list_of(Type::int_()));
    expect_not_subtype(Type::range_(), Type::list_of(Type::int_()));
}

// None and Range have no arm of their own: identity and the Object rule cover
// them. Asserted so a later reader does not add a redundant arm.
TEST(IsSubtype, NoneAndRangeFallOutOfIdentityAndTop) {
    expect_subtype(Type::none(), Type::none());
    expect_subtype(Type::range_(), Type::range_());
    expect_not_subtype(Type::none(), Type::str());
    expect_not_subtype(Type::str(), Type::none());
    expect_not_subtype(Type::range_(), Type::int_());
}

TEST(NumericRank, OrdersTheTowerAndExcludesEverythingElse) {
    EXPECT_EQ(numeric_rank(TypeKind::Bool), 1);
    EXPECT_EQ(numeric_rank(TypeKind::Int), 2);
    EXPECT_EQ(numeric_rank(TypeKind::Float), 3);
    EXPECT_EQ(numeric_rank(TypeKind::Complex), 4);

    EXPECT_EQ(numeric_rank(TypeKind::Unknown), 0);
    EXPECT_EQ(numeric_rank(TypeKind::NoneType), 0);
    EXPECT_EQ(numeric_rank(TypeKind::Str), 0);
    EXPECT_EQ(numeric_rank(TypeKind::List), 0);
    EXPECT_EQ(numeric_rank(TypeKind::Object), 0);
    EXPECT_EQ(numeric_rank(TypeKind::Class), 0);
}

TEST(IsSubtype, AMemberIsAssignableToItsUnion) {
    const Type optional_str = Type::union_of({Type::str(), Type::none()});

    expect_subtype(Type::str(), optional_str);
    expect_subtype(Type::none(), optional_str);
    expect_not_subtype(Type::int_(), optional_str);
}

TEST(IsSubtype, AUnionIsAssignableOnlyWhenEveryMemberIs) {
    const Type int_or_str = Type::union_of({Type::int_(), Type::str()});

    expect_subtype(int_or_str, Type::object());
    expect_not_subtype(int_or_str, Type::int_());
    expect_not_subtype(int_or_str, Type::str());
}

TEST(IsSubtype, AUnionIsAssignableToAWiderUnion) {
    const Type narrow = Type::union_of({Type::int_(), Type::str()});
    const Type wide = Type::union_of({Type::int_(), Type::str(), Type::none()});

    expect_subtype(narrow, wide);
    expect_not_subtype(wide, narrow);
}

// The documented counterpart to Type's order-sensitive operator==: two
// unions spelled in different orders are mutually assignable even though
// they are not equal.
TEST(IsSubtype, UnionMemberOrderDoesNotAffectCompatibility) {
    const Type left = Type::union_of({Type::int_(), Type::str()});
    const Type right = Type::union_of({Type::str(), Type::int_()});

    EXPECT_NE(left, right);
    expect_subtype(left, right);
    expect_subtype(right, left);
}

TEST(IsSubtype, TheNumericTowerAppliesThroughAUnion) {
    const Type int_or_none = Type::union_of({Type::int_(), Type::none()});

    expect_subtype(Type::bool_(), int_or_none);
    expect_subtype(Type::int_(), Type::union_of({Type::float_(), Type::none()}));
}

TEST(IsSubtype, CallableReturnTypesAreCovariant) {
    expect_subtype(Type::callable({Type::int_()}, Type::bool_()),
                   Type::callable({Type::int_()}, Type::int_()));
    expect_not_subtype(Type::callable({Type::int_()}, Type::int_()),
                       Type::callable({Type::int_()}, Type::bool_()));
}

// Fails in the covariant reading, which is the point: a function that only
// accepts int cannot stand in where one accepting bool-or-int is expected...
// but one accepting the wider float DOES stand in where int is expected.
TEST(IsSubtype, CallableParameterTypesAreContravariant) {
    expect_subtype(Type::callable({Type::float_()}, Type::int_()),
                   Type::callable({Type::int_()}, Type::int_()));
    expect_not_subtype(Type::callable({Type::bool_()}, Type::int_()),
                       Type::callable({Type::int_()}, Type::int_()));
}

TEST(IsSubtype, CallableArityMustMatch) {
    expect_not_subtype(Type::callable({Type::int_()}, Type::int_()),
                       Type::callable({Type::int_(), Type::int_()}, Type::int_()));
    expect_not_subtype(Type::callable({}, Type::int_()),
                       Type::callable({Type::int_()}, Type::int_()));
    expect_subtype(Type::callable({}, Type::int_()), Type::callable({}, Type::int_()));
}

TEST(IsSubtype, AClassIsAssignableToItsDirectBase) {
    const semantic_test_support::FakeClassLookup classes({{"Base", {}}, {"Sub", {"Base"}}});

    EXPECT_TRUE(is_subtype(Type::class_of("Sub"), Type::class_of("Base"), &classes));
    EXPECT_FALSE(is_subtype(Type::class_of("Base"), Type::class_of("Sub"), &classes));
}

TEST(IsSubtype, AClassIsAssignableUpAChainOfBases) {
    const semantic_test_support::FakeClassLookup classes(
        {{"Top", {}}, {"Middle", {"Top"}}, {"Bottom", {"Middle"}}});

    EXPECT_TRUE(is_subtype(Type::class_of("Bottom"), Type::class_of("Top"), &classes));
    EXPECT_TRUE(is_subtype(Type::class_of("Bottom"), Type::class_of("Middle"), &classes));
    EXPECT_FALSE(is_subtype(Type::class_of("Top"), Type::class_of("Bottom"), &classes));
}

TEST(IsSubtype, AClassIsAssignableThroughAnyOfSeveralBases) {
    const semantic_test_support::FakeClassLookup classes(
        {{"Left", {}}, {"Right", {}}, {"Both", {"Left", "Right"}}});

    EXPECT_TRUE(is_subtype(Type::class_of("Both"), Type::class_of("Left"), &classes));
    EXPECT_TRUE(is_subtype(Type::class_of("Both"), Type::class_of("Right"), &classes));
}

TEST(IsSubtype, ClassesAreUnrelatedWithNoLookup) {
    expect_not_subtype(Type::class_of("Sub"), Type::class_of("Base"));
    // Identity still holds without a lookup.
    expect_subtype(Type::class_of("Base"), Type::class_of("Base"));
    expect_subtype(Type::class_of("Base"), Type::object());
}

// A malformed table must not hang the compiler. Python rejects this, but
// nothing guarantees the table is well-formed, and a recursive walk would
// not return.
TEST(IsSubtype, ACycleInTheBaseChainTerminates) {
    const semantic_test_support::FakeClassLookup classes({{"A", {"B"}}, {"B", {"A"}}});

    EXPECT_FALSE(is_subtype(Type::class_of("A"), Type::class_of("Unrelated"), &classes));
    EXPECT_TRUE(is_subtype(Type::class_of("A"), Type::class_of("B"), &classes));
}

TEST(IsSubtype, ClassSubtypingWorksInsideAUnionAndATuple) {
    const semantic_test_support::FakeClassLookup classes({{"Base", {}}, {"Sub", {"Base"}}});

    EXPECT_TRUE(is_subtype(Type::class_of("Sub"),
                           Type::union_of({Type::class_of("Base"), Type::none()}), &classes));
    EXPECT_TRUE(is_subtype(Type::tuple_of({Type::class_of("Sub")}),
                           Type::tuple_of({Type::class_of("Base")}), &classes));
}

TEST(IsSubtype, AClassIsNotAssignableToAnUnrelatedKind) {
    const semantic_test_support::FakeClassLookup classes({{"Widget", {}}});

    EXPECT_FALSE(is_subtype(Type::class_of("Widget"), Type::int_(), &classes));
    EXPECT_FALSE(is_subtype(Type::int_(), Type::class_of("Widget"), &classes));
}

} // namespace
} // namespace cythonpp::domain::semantic
