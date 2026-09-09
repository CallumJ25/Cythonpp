#include <optional>
#include <string>

#include <gtest/gtest.h>

#include "domain/semantic/builtin_type_names.h"
#include "domain/semantic/type_kind.h"

namespace cythonpp::domain::semantic {
namespace {

TEST(BuiltinTypeKind, MapsEveryScalarSpelling) {
    EXPECT_EQ(builtin_type_kind("bool"), TypeKind::Bool);
    EXPECT_EQ(builtin_type_kind("int"), TypeKind::Int);
    EXPECT_EQ(builtin_type_kind("float"), TypeKind::Float);
    EXPECT_EQ(builtin_type_kind("complex"), TypeKind::Complex);
    EXPECT_EQ(builtin_type_kind("str"), TypeKind::Str);
    EXPECT_EQ(builtin_type_kind("bytes"), TypeKind::Bytes);
    EXPECT_EQ(builtin_type_kind("bytearray"), TypeKind::ByteArray);
    EXPECT_EQ(builtin_type_kind("range"), TypeKind::Range);
    EXPECT_EQ(builtin_type_kind("object"), TypeKind::Object);
}

TEST(BuiltinTypeKind, MapsEveryGenericSpelling) {
    EXPECT_EQ(builtin_type_kind("list"), TypeKind::List);
    EXPECT_EQ(builtin_type_kind("dict"), TypeKind::Dict);
    EXPECT_EQ(builtin_type_kind("set"), TypeKind::Set);
    EXPECT_EQ(builtin_type_kind("frozenset"), TypeKind::FrozenSet);
    EXPECT_EQ(builtin_type_kind("tuple"), TypeKind::Tuple);
}

// `print` is a FUNCTION, not a type. mypy rejects `x: print`, and this table
// is what makes that rejection correct rather than accidental -- Task 7 seeds
// the class table from builtins filtered to isinstance(x, type), which
// excludes print for the same reason.
TEST(BuiltinTypeKind, IsEmptyForNamesThatAreNotBuiltinTypes) {
    EXPECT_EQ(builtin_type_kind("print"), std::nullopt);
    EXPECT_EQ(builtin_type_kind("len"), std::nullopt);
    EXPECT_EQ(builtin_type_kind("Widget"), std::nullopt);
    EXPECT_EQ(builtin_type_kind(""), std::nullopt);
    // Exception classes are SEEDED CLASSES (Task 7), not model kinds. There is
    // no TypeKind::Exception and there should not be.
    EXPECT_EQ(builtin_type_kind("Exception"), std::nullopt);
    EXPECT_EQ(builtin_type_kind("type"), std::nullopt);
}

TEST(BuiltinTypeArity, ReportsTheRequiredParameterCount) {
    EXPECT_EQ(builtin_type_arity("int"), 0);
    EXPECT_EQ(builtin_type_arity("list"), 1);
    EXPECT_EQ(builtin_type_arity("set"), 1);
    EXPECT_EQ(builtin_type_arity("frozenset"), 1);
    EXPECT_EQ(builtin_type_arity("dict"), 2);
    EXPECT_EQ(builtin_type_arity("tuple"), kVariadicArity);
}

TEST(BuiltinTypeArity, IsZeroForAnUnknownName) {
    EXPECT_EQ(builtin_type_arity("Widget"), 0);
}

// Derived from one table, so every spelling round-trips through both
// directions. A hand-written second table is what would let them drift.
TEST(BuiltinTypeNames, SpellingRoundTripsThroughKind) {
    for (const char* spelling : {"bool", "bytearray", "bytes", "complex", "dict", "float",
                                 "frozenset", "int", "list", "object", "range", "set", "str",
                                 "tuple"}) {
        const std::optional<TypeKind> kind = builtin_type_kind(spelling);
        ASSERT_TRUE(kind.has_value()) << spelling;
        EXPECT_EQ(builtin_type_spelling(*kind), std::optional<std::string>(spelling)) << spelling;
    }
}

TEST(BuiltinTypeNames, NonBuiltinKindsHaveNoSpelling) {
    EXPECT_FALSE(builtin_type_spelling(TypeKind::Unknown).has_value());
    EXPECT_FALSE(builtin_type_spelling(TypeKind::Union).has_value());
    EXPECT_FALSE(builtin_type_spelling(TypeKind::Callable).has_value());
    EXPECT_FALSE(builtin_type_spelling(TypeKind::Class).has_value());
    EXPECT_FALSE(builtin_type_spelling(TypeKind::Ellipsis).has_value());
}

} // namespace
} // namespace cythonpp::domain::semantic
