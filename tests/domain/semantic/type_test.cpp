#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "domain/semantic/type.h"
#include "domain/semantic/type_kind.h"

namespace cythonpp::domain::semantic {
namespace {

TEST(Type, ValueInitializationIsUnknown) {
    const Type type;

    EXPECT_EQ(type.kind, TypeKind::Unknown);
    EXPECT_TRUE(type.name.empty());
    EXPECT_TRUE(type.args.empty());
    EXPECT_EQ(type, Type::unknown());
}

TEST(Type, ScalarFactoriesCarryOnlyTheirKind) {
    EXPECT_EQ(Type::none().kind, TypeKind::NoneType);
    EXPECT_EQ(Type::bool_().kind, TypeKind::Bool);
    EXPECT_EQ(Type::int_().kind, TypeKind::Int);
    EXPECT_EQ(Type::float_().kind, TypeKind::Float);
    EXPECT_EQ(Type::complex_().kind, TypeKind::Complex);
    EXPECT_EQ(Type::str().kind, TypeKind::Str);
    EXPECT_EQ(Type::bytes().kind, TypeKind::Bytes);
    EXPECT_EQ(Type::bytearray_().kind, TypeKind::ByteArray);
    EXPECT_EQ(Type::ellipsis().kind, TypeKind::Ellipsis);
    EXPECT_EQ(Type::range_().kind, TypeKind::Range);
    EXPECT_EQ(Type::object().kind, TypeKind::Object);
    EXPECT_TRUE(Type::int_().args.empty());
}

TEST(Type, ContainerFactoriesHoldTheirArguments) {
    const Type list = Type::list_of(Type::int_());
    ASSERT_EQ(list.args.size(), 1u);
    EXPECT_EQ(list.kind, TypeKind::List);
    EXPECT_EQ(list.args.front(), Type::int_());

    const Type dict = Type::dict_of(Type::str(), Type::int_());
    ASSERT_EQ(dict.args.size(), 2u);
    EXPECT_EQ(dict.args.front(), Type::str());
    EXPECT_EQ(dict.args.back(), Type::int_());

    EXPECT_EQ(Type::set_of(Type::int_()).kind, TypeKind::Set);
    EXPECT_EQ(Type::frozenset_of(Type::int_()).kind, TypeKind::FrozenSet);
    EXPECT_EQ(Type::tuple_of({Type::int_(), Type::str()}).args.size(), 2u);
}

TEST(Type, NestsToArbitraryDepth) {
    const Type nested = Type::dict_of(Type::str(), Type::list_of(Type::set_of(Type::int_())));

    ASSERT_EQ(nested.args.size(), 2u);
    ASSERT_EQ(nested.args.back().args.size(), 1u);
    ASSERT_EQ(nested.args.back().args.front().args.size(), 1u);
    EXPECT_EQ(nested.args.back().args.front().args.front(), Type::int_());
    EXPECT_EQ(nested, nested);
}

TEST(Type, CallableStoresItsReturnTypeLast) {
    const Type function = Type::callable({Type::int_(), Type::str()}, Type::bool_());

    ASSERT_EQ(function.args.size(), 3u);
    EXPECT_EQ(function.kind, TypeKind::Callable);
    EXPECT_EQ(function.args[0], Type::int_());
    EXPECT_EQ(function.args[1], Type::str());
    EXPECT_EQ(function.args.back(), Type::bool_());
}

TEST(Type, AZeroParameterCallableHasOneArgument) {
    const Type function = Type::callable({}, Type::none());

    ASSERT_EQ(function.args.size(), 1u);
    EXPECT_EQ(function.args.back(), Type::none());
}

TEST(Type, ClassCarriesItsName) {
    const Type user = Type::class_of("Widget");

    EXPECT_EQ(user.kind, TypeKind::Class);
    EXPECT_EQ(user.name, "Widget");
    EXPECT_NE(user, Type::class_of("Gadget"));
    EXPECT_EQ(user, Type::class_of("Widget"));
}

TEST(Type, EqualityComparesKindNameAndArguments) {
    EXPECT_EQ(Type::list_of(Type::int_()), Type::list_of(Type::int_()));
    EXPECT_NE(Type::list_of(Type::int_()), Type::list_of(Type::str()));
    EXPECT_NE(Type::list_of(Type::int_()), Type::set_of(Type::int_()));
    EXPECT_NE(Type::int_(), Type::float_());
    EXPECT_NE(Type::dict_of(Type::str(), Type::int_()), Type::dict_of(Type::int_(), Type::str()));
}

TEST(Type, UnionOfTwoDistinctMembersIsAUnion) {
    const Type optional = Type::union_of({Type::str(), Type::none()});

    EXPECT_EQ(optional.kind, TypeKind::Union);
    ASSERT_EQ(optional.args.size(), 2u);
    EXPECT_EQ(optional.args.front(), Type::str());
    EXPECT_EQ(optional.args.back(), Type::none());
}

TEST(Type, UnionOfDropsDuplicates) {
    const Type collapsed = Type::union_of({Type::int_(), Type::int_()});

    EXPECT_EQ(collapsed, Type::int_());
    EXPECT_NE(collapsed.kind, TypeKind::Union);
}

TEST(Type, UnionOfASingleMemberCollapsesToThatMember) {
    EXPECT_EQ(Type::union_of({Type::str()}), Type::str());
}

TEST(Type, UnionOfNothingIsUnknown) {
    EXPECT_EQ(Type::union_of({}), Type::unknown());
}

TEST(Type, UnionOfFlattensNestedUnions) {
    const Type inner = Type::union_of({Type::int_(), Type::str()});
    const Type outer = Type::union_of({inner, Type::none()});

    EXPECT_EQ(outer.kind, TypeKind::Union);
    ASSERT_EQ(outer.args.size(), 3u);
    EXPECT_EQ(outer.args[0], Type::int_());
    EXPECT_EQ(outer.args[1], Type::str());
    EXPECT_EQ(outer.args[2], Type::none());
    for (const Type& member : outer.args) {
        EXPECT_NE(member.kind, TypeKind::Union) << "a union's members must never be unions";
    }
}

TEST(Type, UnionOfDeduplicatesAcrossFlattening) {
    const Type inner = Type::union_of({Type::int_(), Type::str()});
    const Type outer = Type::union_of({inner, Type::int_()});

    ASSERT_EQ(outer.args.size(), 2u);
    EXPECT_EQ(outer.args[0], Type::int_());
    EXPECT_EQ(outer.args[1], Type::str());
}

// Pins the documented asymmetry: == is exact and order-sensitive, and
// is_subtype (Task 4) is not. A reader who assumes unions are canonicalised
// will be surprised here rather than in production.
TEST(Type, UnionMemberOrderIsPreservedAndSignificantToEquality) {
    const Type left = Type::union_of({Type::int_(), Type::str()});
    const Type right = Type::union_of({Type::str(), Type::int_()});

    EXPECT_NE(left, right);
    EXPECT_EQ(left.args.front(), Type::int_());
    EXPECT_EQ(right.args.front(), Type::str());
}

// Unknown is absorbing in a union too, not just at is_subtype: a member that
// failed to resolve must not survive into `int | Unknown`, or is_subtype
// would compare it as only partially compatible with everything and a single
// root cause would draw a second diagnostic at every later use.
TEST(Type, UnionOfAbsorbsUnknown) {
    EXPECT_EQ(Type::union_of({Type::int_(), Type::unknown()}), Type::unknown());
    EXPECT_EQ(Type::union_of({Type::unknown(), Type::str(), Type::none()}), Type::unknown());
    EXPECT_EQ(Type::union_of({Type::unknown(), Type::unknown()}), Type::unknown());
}

TEST(Type, IsCopyableUnlikeAnAstNode) {
    const Type original = Type::dict_of(Type::str(), Type::list_of(Type::int_()));
    const Type copy = original;

    EXPECT_EQ(copy, original);
}

// A Callable remembers how many of its trailing parameters are optional, so
// the arity check at a call site can tell `def f(a: int, b: int = 1)` (one
// required argument) from `def f(a: int, b: int)` (two). Nothing else about
// the two signatures differs, which is exactly why the count has to live on
// the Type and be part of its identity.
TEST(Type, ACallableRemembersItsDefaultedParameterCount) {
    const Type no_defaults = Type::callable({Type::int_(), Type::int_()}, Type::none());
    const Type one_default = Type::callable({Type::int_(), Type::int_()}, Type::none(), 1);

    EXPECT_EQ(no_defaults.defaulted_params, 0u);
    EXPECT_EQ(one_default.defaulted_params, 1u);
    EXPECT_NE(no_defaults, one_default);
    EXPECT_EQ(one_default.args.size(), 3u) << "params then the return, return last";
    EXPECT_EQ(one_default.args.back(), Type::none());
}

// Omitting the argument means "every parameter is required", which is the
// right reading for a signature with no defaults AND for a
// `Callable[[int], str]` annotation, whose spelling cannot express an
// optional parameter at all.
TEST(Type, ACallableWithoutADefaultedCountRequiresEveryParameter) {
    EXPECT_EQ(Type::callable({Type::int_()}, Type::str()).defaulted_params, 0u);
    EXPECT_EQ(Type::unknown().defaulted_params, 0u);
    EXPECT_EQ(Type::list_of(Type::int_()).defaulted_params, 0u);
}

} // namespace
} // namespace cythonpp::domain::semantic
