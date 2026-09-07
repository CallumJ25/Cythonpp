#include <vector>

#include <gtest/gtest.h>

#include "domain/semantic/type.h"
#include "domain/semantic/type_name.h"

namespace cythonpp::domain::semantic {
namespace {

TEST(TypeName, RendersEveryScalarKind) {
    EXPECT_EQ(type_name(Type::unknown()), "Unknown");
    EXPECT_EQ(type_name(Type::none()), "None");
    EXPECT_EQ(type_name(Type::bool_()), "bool");
    EXPECT_EQ(type_name(Type::int_()), "int");
    EXPECT_EQ(type_name(Type::float_()), "float");
    EXPECT_EQ(type_name(Type::complex_()), "complex");
    EXPECT_EQ(type_name(Type::str()), "str");
    EXPECT_EQ(type_name(Type::bytes()), "bytes");
    EXPECT_EQ(type_name(Type::bytearray_()), "bytearray");
    EXPECT_EQ(type_name(Type::ellipsis()), "ellipsis");
    EXPECT_EQ(type_name(Type::range_()), "range");
    EXPECT_EQ(type_name(Type::object()), "object");
}

TEST(TypeName, RendersParameterisedContainers) {
    EXPECT_EQ(type_name(Type::list_of(Type::int_())), "list[int]");
    EXPECT_EQ(type_name(Type::set_of(Type::str())), "set[str]");
    EXPECT_EQ(type_name(Type::frozenset_of(Type::bool_())), "frozenset[bool]");
    EXPECT_EQ(type_name(Type::dict_of(Type::str(), Type::int_())), "dict[str, int]");
    EXPECT_EQ(type_name(Type::tuple_of({Type::int_(), Type::str()})), "tuple[int, str]");
}

TEST(TypeName, RendersTheEmptyTupleAsMypyDoes) {
    EXPECT_EQ(type_name(Type::tuple_of({})), "tuple[()]");
}

TEST(TypeName, NestsRecursively) {
    EXPECT_EQ(type_name(Type::dict_of(Type::str(), Type::list_of(Type::set_of(Type::int_())))),
              "dict[str, list[set[int]]]");
}

TEST(TypeName, RendersAUnionInConstructionOrder) {
    EXPECT_EQ(type_name(Type::union_of({Type::str(), Type::none()})), "str | None");
    EXPECT_EQ(type_name(Type::union_of({Type::none(), Type::str()})), "None | str");
    EXPECT_EQ(type_name(Type::union_of({Type::int_(), Type::str(), Type::none()})),
              "int | str | None");
}

TEST(TypeName, RendersACallableWithItsParameterListBracketed) {
    EXPECT_EQ(type_name(Type::callable({Type::int_()}, Type::str())), "Callable[[int], str]");
    EXPECT_EQ(type_name(Type::callable({Type::int_(), Type::bool_()}, Type::none())),
              "Callable[[int, bool], None]");
}

TEST(TypeName, RendersAZeroParameterCallableWithAnEmptyParameterList) {
    EXPECT_EQ(type_name(Type::callable({}, Type::none())), "Callable[[], None]");
}

TEST(TypeName, RendersAClassAsItsBareName) {
    EXPECT_EQ(type_name(Type::class_of("Widget")), "Widget");
    EXPECT_EQ(type_name(Type::list_of(Type::class_of("Widget"))), "list[Widget]");
}

// Unreachable through the factories, which always supply arguments, but
// type_name is total and a hand-built Type must not render as garbage.
TEST(TypeName, RendersAParameterlessContainerAsItsBareName) {
    Type bare;
    bare.kind = TypeKind::List;

    EXPECT_EQ(type_name(bare), "list");
}

TEST(TypeName, RendersAnArgumentlessCallableAsItsBareName) {
    Type bare;
    bare.kind = TypeKind::Callable;

    EXPECT_EQ(type_name(bare), "Callable");
}

} // namespace
} // namespace cythonpp::domain::semantic
