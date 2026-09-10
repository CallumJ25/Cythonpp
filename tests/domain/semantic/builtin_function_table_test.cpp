#include <string>

#include <gtest/gtest.h>

#include "domain/semantic/builtin_call_table.h"
#include "domain/semantic/builtin_class_table.h"
#include "domain/semantic/builtin_function_table.h"
#include "domain/semantic/builtin_type_names.h"

namespace cythonpp::domain::semantic {
namespace {

TEST(BuiltinFunctionTable, ContainsTheOrdinaryBuiltinFunctions) {
    for (const char* name : {"abs", "hash", "len", "print", "sorted", "getattr", "isinstance"}) {
        EXPECT_TRUE(is_builtin_function_name(name)) << name;
    }
}

// THE COMPLEMENT PROPERTY, and it is the reason this file can be generated
// separately from builtin_class_table.h without either shadowing the other:
// the two extractions use opposite filters, so no name is in both.
TEST(BuiltinFunctionTable, NoNameIsAlsoABuiltinClass) {
    for (std::size_t index = 0; index < kBuiltinFunctionCount; ++index) {
        const std::string name = kBuiltinFunctions[index];
        for (std::size_t class_index = 0; class_index < kBuiltinClassCount; ++class_index) {
            EXPECT_NE(name, kBuiltinClasses[class_index].name)
                << name << " is recorded as both a builtin function and a builtin class";
        }
    }
}

// A model KIND is not a function either -- `int` must keep resolving through
// the kind carve-out, not through this table.
TEST(BuiltinFunctionTable, NoNameIsAlsoAModelKind) {
    for (std::size_t index = 0; index < kBuiltinFunctionCount; ++index) {
        EXPECT_FALSE(builtin_type_kind(kBuiltinFunctions[index]).has_value())
            << kBuiltinFunctions[index];
    }
}

TEST(BuiltinFunctionTable, DoesNotContainClassNames) {
    for (const char* name : {"int", "str", "list", "dict", "zip", "Exception", "object"}) {
        EXPECT_FALSE(is_builtin_function_name(name)) << name;
    }
}

// Paired with a TRUE assertion in the same test, so a predicate hardwired to
// always return false cannot pass this test silently -- a pure `EXPECT_FALSE`
// guard has no failure mode if the real body underneath is never reached.
TEST(BuiltinFunctionTable, DoesNotContainAnArbitraryName) {
    EXPECT_TRUE(is_builtin_function_name("len"));
    EXPECT_FALSE(is_builtin_function_name("definitely_not_a_builtin"));
    EXPECT_FALSE(is_builtin_function_name(""));
}

} // namespace
} // namespace cythonpp::domain::semantic
