#include <gtest/gtest.h>

#include "domain/semantic/type.h"
#include "domain/semantic/type_compatibility.h"
#include "domain/semantic/type_name.h"

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

} // namespace
} // namespace cythonpp::domain::semantic
