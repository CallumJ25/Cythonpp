#include <gtest/gtest.h>

#include "domain/codegen/cpp_type_name.h"
#include "domain/semantic/type.h"

namespace cythonpp::domain::codegen {
namespace {

TEST(CppTypeName, ScalarsInTheSliceMapToRuntimeTypes) {
    EXPECT_EQ(cpp_type_name(semantic::Type::int_()).value(), "py::int_");
    EXPECT_EQ(cpp_type_name(semantic::Type::float_()).value(), "py::float_");
    EXPECT_EQ(cpp_type_name(semantic::Type::bool_()).value(), "py::bool_");
    EXPECT_EQ(cpp_type_name(semantic::Type::str()).value(), "py::str");
    EXPECT_EQ(cpp_type_name(semantic::Type::none()).value(), "py::none_t");
}

// Unknown is absorbing in the type model, so a value carrying it has no
// representable C++ type and must never be guessed at.
TEST(CppTypeName, UnknownHasNoMapping) {
    EXPECT_FALSE(cpp_type_name(semantic::Type::unknown()).has_value());
}

// A union is what `and`/`or` over differing operand types produces. No C++
// type represents it in this slice, which is what makes the emitter refuse
// those programs rather than inventing something.
TEST(CppTypeName, TypesOutsideTheSliceHaveNoMapping) {
    EXPECT_FALSE(cpp_type_name(semantic::Type::union_of({semantic::Type::int_(),
                                                         semantic::Type::str()}))
                     .has_value());
    EXPECT_FALSE(cpp_type_name(semantic::Type::list_of(semantic::Type::int_())).has_value());
    EXPECT_FALSE(cpp_type_name(semantic::Type::complex_()).has_value());
    EXPECT_FALSE(cpp_type_name(semantic::Type::object()).has_value());
}

} // namespace
} // namespace cythonpp::domain::codegen
