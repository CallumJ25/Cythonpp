#include <cstddef>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "domain/semantic/builtin_object_member_table.h"
#include "domain/semantic/class_table.h"

namespace cythonpp::domain::semantic {
namespace {

// Pins the GENERATED table verbatim, the same way
// builtin_class_table_test.cpp's BoundedConstructorAritiesMatchMypy pins the
// arity bands: re-derived directly against mypy 1.18.1 (see
// scripts/verify_corpus_labels.py's object_member_names, and this file's own
// header comment), not copied from any earlier count. 19 of dir(object)'s 24
// names -- every one mypy --strict does not reject when read as a bare
// attribute off a fresh no-base instance.
TEST(BuiltinObjectMemberTable, MatchesTheMypyFilteredDirOfObject) {
    const std::vector<std::string> expected = {
        "__class__",         "__delattr__", "__dir__",       "__doc__",
        "__eq__",            "__format__",  "__getattribute__", "__getstate__",
        "__hash__",          "__init_subclass__", "__ne__",  "__new__",
        "__reduce__",        "__reduce_ex__", "__repr__",    "__setattr__",
        "__sizeof__",        "__str__",     "__subclasshook__",
    };
    ASSERT_EQ(kObjectMemberCount, expected.size());
    for (std::size_t index = 0; index < kObjectMemberCount; ++index) {
        EXPECT_EQ(std::string(kObjectMembers[index]), expected[index]) << "at index " << index;
    }
}

// The five dir(object) names deliberately EXCLUDED, and why each one is:
// __init__ (mypy: unsound instance access, [misc]) and __lt__/__le__/__gt__/
// __ge__ (typeshed's object omits them; mypy's --strict verdict on a bare
// read is "Unsupported left operand type", [operator]) -- both are cases
// where mypy rejects and CPython accepts, so the union rule says reject.
TEST(BuiltinObjectMemberTable, ExcludesTheFiveMypyRejectedDunders) {
    for (const char* name : {"__init__", "__lt__", "__le__", "__gt__", "__ge__"}) {
        EXPECT_FALSE(ClassTable::is_object_member(name)) << name;
    }
}

// __dict__ and __module__ are NOT dir(object) names at all (object() itself
// raises AttributeError for both at runtime) -- they belong to the ordinary
// class machinery, not to object's own member set, and must not appear here.
TEST(BuiltinObjectMemberTable, DoesNotContainTheClassMachineryNames) {
    for (const char* name : {"__dict__", "__module__", "__weakref__", "__annotations__"}) {
        EXPECT_FALSE(ClassTable::is_object_member(name)) << name;
    }
}

} // namespace
} // namespace cythonpp::domain::semantic
