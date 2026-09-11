#include <optional>
#include <string>

#include <gtest/gtest.h>

#include "domain/semantic/builtin_class_genericity.h"

namespace cythonpp::domain::semantic {
namespace {

// The tri-state contract, pinned per value, because the CALLER's routing
// depends on all three being distinguishable: true -> NotImplementedError,
// false -> a genuine `not subscriptable` TypeError, nullopt -> the same
// TypeError but on different evidence (a class the program itself declares).
// Collapsing nullopt into either of the other two is a defect in one
// direction or the other, so each is asserted separately rather than through
// a bool.

TEST(BuiltinClassGenericity, ReturnsTrueForAGenericBuiltinClass) {
    for (const std::string& name : {"type", "slice", "memoryview", "ExceptionGroup",
                                    "BaseExceptionGroup", "zip", "map", "filter",
                                    "enumerate", "reversed", "staticmethod", "classmethod",
                                    "list", "dict", "set", "frozenset", "tuple"}) {
        const std::optional<bool> answer = builtin_class_accepts_type_arguments(name);
        ASSERT_TRUE(answer.has_value()) << name;
        EXPECT_TRUE(*answer) << name;
    }
}

TEST(BuiltinClassGenericity, ReturnsFalseForANonGenericBuiltinClass) {
    for (const std::string& name : {"ValueError", "Exception", "BaseException", "OSError",
                                    "IOError", "int", "str", "object", "super", "property",
                                    "range", "bool", "bytes", "bytearray", "complex", "float"}) {
        const std::optional<bool> answer = builtin_class_accepts_type_arguments(name);
        ASSERT_TRUE(answer.has_value()) << name;
        EXPECT_FALSE(*answer) << name;
    }
}

// nullopt, NOT false -- the distinction the resolver's second evidence arm
// rests on. A builtin FUNCTION and a user class are both "not a builtin
// class", and neither may be read as "a builtin class that takes no type
// arguments", because only the table can make that claim.
TEST(BuiltinClassGenericity, ReturnsNulloptForANameThatIsNotABuiltinClass) {
    for (const std::string& name : {"Widget", "C", "print", "len", "__loader__",
                                    "_IncompleteInputError", "", "Local"}) {
        EXPECT_FALSE(builtin_class_accepts_type_arguments(name).has_value()) << name;
    }
}

} // namespace
} // namespace cythonpp::domain::semantic
