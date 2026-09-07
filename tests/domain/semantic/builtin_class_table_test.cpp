#include <cstddef>
#include <string>

#include <gtest/gtest.h>

#include "domain/semantic/builtin_class_table.h"

namespace cythonpp::domain::semantic {
namespace {

const BuiltinClass* find(const std::string& name) {
    for (std::size_t index = 0; index < kBuiltinClassCount; ++index) {
        if (name == kBuiltinClasses[index].name) {
            return &kBuiltinClasses[index];
        }
    }
    return nullptr;
}

// The five names from Spec 5a's recorded obligation, each mypy-clean and each
// previously drawing a false NameError.
TEST(BuiltinClassTable, ContainsTheNamesTheRecordedObligationNamed) {
    for (const char* name : {"type", "Exception", "BaseException", "slice", "memoryview"}) {
        EXPECT_NE(find(name), nullptr) << name << " must be seeded";
    }
}

// Bases are the half that stops one fix from opening another hole.
TEST(BuiltinClassTable, CarriesTheExceptionHierarchy) {
    const BuiltinClass* value_error = find("ValueError");
    ASSERT_NE(value_error, nullptr);
    EXPECT_STREQ(value_error->bases[0], "Exception");

    const BuiltinClass* root = find("BaseException");
    ASSERT_NE(root, nullptr);
    // object is not recorded: the model spells the top type as
    // TypeKind::Object, not as a class.
    EXPECT_EQ(root->bases[0], nullptr);
}

// `print` is a function. Its absence keeps `x: print` a NameError, which mypy
// also reports -- so the rejection stays correct rather than accidental.
TEST(BuiltinClassTable, ExcludesBuiltinFunctions) {
    EXPECT_EQ(find("print"), nullptr);
    EXPECT_EQ(find("len"), nullptr);
    EXPECT_EQ(find("sorted"), nullptr);
    EXPECT_EQ(find("abs"), nullptr);
}

TEST(BuiltinClassTable, ExcludesPrivateNames) {
    EXPECT_EQ(find("__loader__"), nullptr);
    EXPECT_EQ(find("_IncompleteInputError"), nullptr);
}

// Every base named by any entry must itself be an entry, or ClassTable's
// transitive walk hits a dangling name and silently stops.
TEST(BuiltinClassTable, EveryNamedBaseIsItselfAnEntry) {
    for (std::size_t index = 0; index < kBuiltinClassCount; ++index) {
        for (const char* base : kBuiltinClasses[index].bases) {
            if (base == nullptr) {
                continue;
            }
            EXPECT_NE(find(base), nullptr)
                << kBuiltinClasses[index].name << " names base " << base
                << ", which is not itself an entry";
        }
    }
}

// The generic seven, whose bare use is a mypy type-arg error we deliberately
// miss but whose SUBSCRIPTED use is mypy-clean -- so they must be present, or
// `x: zip[int]` would draw a NameError instead of the NotImplementedError
// Task 9 gives it.
TEST(BuiltinClassTable, ContainsTheGenericBuiltinClasses) {
    for (const char* name : {"zip", "map", "filter", "enumerate", "reversed",
                             "staticmethod", "classmethod"}) {
        EXPECT_NE(find(name), nullptr) << name << " must be seeded";
    }
}

} // namespace
} // namespace cythonpp::domain::semantic
