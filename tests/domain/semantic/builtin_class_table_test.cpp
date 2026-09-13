#include <cstddef>
#include <map>
#include <set>
#include <string>
#include <utility>

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

// THE MEASUREMENT, PINNED. `accepts_type_arguments` is a claim about typeshed,
// re-derived by the generator by running `mypy --strict` over one
// `def f(a: NAME[int]) -> None` line per class name. ctest cannot run mypy, so
// this test is where the answer that was measured is written down: exactly
// these SEVENTEEN names must be true and every other entry false.
//
// Measured 2026-09-11 with mypy 1.18.1 (compiled: yes) / Python 3.14.2.
// A name is false iff mypy answers `"NAME" expects no type arguments`; the
// seventeen below answer otherwise. Verbatim, for the five that are not
// simply clean:
//   "classmethod" expects 3 type arguments, but 1 given  [type-arg]
//   "staticmethod" expects 2 type arguments, but 1 given  [type-arg]
//   "dict" expects 2 type arguments, but 1 given  [type-arg]
//   Type argument "int" of "ExceptionGroup" must be a subtype of "Exception"  [type-var]
//   Type argument "int" of "BaseExceptionGroup" must be a subtype of "BaseException"  [type-var]
// The other twelve (enumerate, filter, frozenset, list, map, memoryview,
// reversed, set, slice, tuple, type, zip) are `Success: no issues found`.
//
// This is the guard against a REGENERATION silently moving an answer, which
// is the only failure mode left once the field is generated -- five of these
// names (type, slice, memoryview, ExceptionGroup, BaseExceptionGroup) were
// missing from the seven-name hand list this field replaced, and each was a
// false `TypeError: 'X' is not subscriptable` on a program both oracles
// accept.
TEST(BuiltinClassTable, AcceptsTypeArgumentsIsTrueForExactlyTheSeventeenGenericNames) {
    const std::set<std::string> generic = {
        "BaseExceptionGroup", "ExceptionGroup", "classmethod", "dict", "enumerate",
        "filter",             "frozenset",      "list",        "map", "memoryview",
        "reversed",           "set",            "slice",       "staticmethod",
        "tuple",              "type",           "zip",
    };
    std::set<std::string> recorded;
    for (std::size_t index = 0; index < kBuiltinClassCount; ++index) {
        if (kBuiltinClasses[index].accepts_type_arguments) {
            recorded.insert(kBuiltinClasses[index].name);
        }
    }
    EXPECT_EQ(recorded, generic);

    // Spot-checks in the other direction, so a wholesale `= true` on every
    // row cannot pass by making both sets equal to the whole table.
    for (const char* name : {"ValueError", "Exception", "BaseException", "int", "str",
                             "object", "super", "property", "range", "bytes"}) {
        const BuiltinClass* entry = find(name);
        ASSERT_NE(entry, nullptr) << name;
        EXPECT_FALSE(entry->accepts_type_arguments) << name;
    }
}

// Pinned verbatim from `mypy --strict` 1.18.1, 2026-09-12, probing each
// class with k arguments of type Any for k in 0..8 and filtering [call-arg].
// ctest cannot run mypy, so this table is the record that the generated
// header matches what mypy actually said.
TEST(BuiltinClassTable, BoundedConstructorAritiesMatchMypy) {
    const std::map<std::string, std::pair<int, int>> expected = {
        {"BaseExceptionGroup", {2, 2}}, {"ExceptionGroup", {2, 2}},
        {"UnicodeDecodeError", {5, 5}}, {"UnicodeEncodeError", {5, 5}},
        {"UnicodeTranslateError", {4, 4}}, {"bool", {0, 1}},
        {"classmethod", {1, 1}}, {"enumerate", {1, 2}},
        {"float", {0, 1}}, {"memoryview", {1, 1}},
        {"object", {0, 0}}, {"property", {0, 4}},
        {"staticmethod", {1, 1}}, {"tuple", {0, 1}},
    };

    std::map<std::string, std::pair<int, int>> actual;
    for (std::size_t i = 0; i < kBuiltinClassCount; ++i) {
        const BuiltinClass& row = kBuiltinClasses[i];
        if (row.max_args != kUnboundedArity) {
            actual.emplace(row.name, std::make_pair(row.min_args, row.max_args));
        }
    }

    EXPECT_EQ(actual, expected);
}

} // namespace
} // namespace cythonpp::domain::semantic
