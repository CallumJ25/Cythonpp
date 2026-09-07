#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "domain/semantic/class_table.h"
#include "domain/semantic/type.h"
#include "domain/semantic/type_compatibility.h"
#include "domain/semantic/type_name.h"

namespace cythonpp::domain::semantic {
namespace {

TEST(ClassTable, SeedsItselfWithBuiltinClasses) {
    const ClassTable table;

    EXPECT_TRUE(table.is_class("Exception"));
    EXPECT_TRUE(table.is_class("BaseException"));
    EXPECT_TRUE(table.is_class("type"));
    EXPECT_TRUE(table.is_class("slice"));
    EXPECT_TRUE(table.is_class("memoryview"));
}

// The reason bases were extracted: e: Exception = ValueError() is mypy-clean.
TEST(ClassTable, SeededClassesCarryTheirBases) {
    const ClassTable table;

    EXPECT_EQ(table.bases_of("ValueError"), (std::vector<std::string>{"Exception"}));
    EXPECT_TRUE(table.bases_of("BaseException").empty());
}

// print is a function, not a type. `x: print` must stay a NameError.
TEST(ClassTable, DoesNotSeedBuiltinFunctions) {
    const ClassTable table;

    EXPECT_FALSE(table.is_class("print"));
    EXPECT_FALSE(table.is_class("len"));
}

TEST(ClassTable, DeclaredClassesAreFound) {
    ClassTable table;
    table.declare("Widget", {});
    table.declare("Button", {"Widget"});

    EXPECT_TRUE(table.is_class("Widget"));
    EXPECT_EQ(table.bases_of("Button"), (std::vector<std::string>{"Widget"}));
    EXPECT_FALSE(table.is_class("Nonexistent"));
    EXPECT_TRUE(table.bases_of("Nonexistent").empty());
}

TEST(ClassTable, NestedClassesAreKeyedByQualifiedName) {
    ClassTable table;
    table.declare("Outer", {});
    table.declare("Outer.Inner", {});

    EXPECT_TRUE(table.is_class("Outer.Inner"));
    // The bare name is NOT a class -- mypy's identity for a nested class is
    // the qualified name, and resolve_attribute already relies on that.
    EXPECT_FALSE(table.is_class("Inner"));
}

TEST(ClassTable, MembersAreFoundOnTheDeclaringClass) {
    ClassTable table;
    table.declare("Widget", {});
    table.declare_member("Widget", "width", Type::int_(), 3);

    const std::optional<Type> width = table.member_type("Widget", "width");
    ASSERT_TRUE(width.has_value());
    EXPECT_EQ(*width, Type::int_());
    EXPECT_EQ(table.member_declared_line("Widget", "width"), 3);

    EXPECT_EQ(table.member_type("Widget", "height"), std::nullopt);
}

TEST(ClassTable, MembersAreFoundThroughTheBaseChain) {
    ClassTable table;
    table.declare("Base", {});
    table.declare_member("Base", "x", Type::int_(), 2);
    table.declare("Middle", {"Base"});
    table.declare("Leaf", {"Middle"});

    const std::optional<Type> inherited = table.member_type("Leaf", "x");
    ASSERT_TRUE(inherited.has_value());
    EXPECT_EQ(*inherited, Type::int_());
}

// Depth-first, left to right. mypy lands here too, and additionally REJECTS
// the conflict with code `misc` -- a check this spec puts out of scope as a
// missed error rather than a false one.
TEST(ClassTable, MultipleInheritanceTakesTheFirstBase) {
    ClassTable table;
    table.declare("A", {});
    table.declare_member("A", "v", Type::int_(), 2);
    table.declare("B", {});
    table.declare_member("B", "v", Type::str(), 5);
    table.declare("D", {"A", "B"});

    const std::optional<Type> resolved = table.member_type("D", "v");
    ASSERT_TRUE(resolved.has_value());
    EXPECT_EQ(*resolved, Type::int_()) << "first base wins";
}

// class A(B) / class B(A) is constructible through declare() and the parser
// cannot reject it. A transitive walk with no visited set infinite-loops, so
// this test is the difference between a hang and a failure.
TEST(ClassTable, ACycleInTheBaseChainTerminates) {
    ClassTable table;
    table.declare("A", {"B"});
    table.declare("B", {"A"});

    EXPECT_EQ(table.member_type("A", "missing"), std::nullopt);
    EXPECT_FALSE(table.inherits_builtin("A"));
}

// Verified: reveal_type(C) for `def __init__(self, a: int) -> None` is
// `def (a: int) -> C` -- self stripped, return replaced by the instance.
TEST(ClassTable, ConstructorTypeStripsSelfAndReturnsTheInstance) {
    ClassTable table;
    table.declare("Widget", {});
    table.declare_method("Widget", "__init__",
                         Type::callable({Type::class_of("Widget"), Type::int_()},
                                        Type::none()));

    EXPECT_EQ(type_name(table.constructor_type("Widget")),
              type_name(Type::callable({Type::int_()}, Type::class_of("Widget"))));
}

// Verified: `class C: pass` then `C(1)` is "Too many arguments for C".
TEST(ClassTable, AClassWithNoInitTakesNoArguments) {
    ClassTable table;
    table.declare("Widget", {});

    EXPECT_EQ(type_name(table.constructor_type("Widget")),
              type_name(Type::callable({}, Type::class_of("Widget"))));
}

// The predicate that forces Task 20's attribute carve-out. Verified:
// `class Sub(int): pass` then Sub().bit_length() is mypy-CLEAN, so an
// attribute miss on Sub must not be a TypeError.
TEST(ClassTable, InheritsBuiltinDetectsABuiltinInTheBaseChain) {
    ClassTable table;
    table.declare("Sub", {"int"});
    table.declare("Deep", {"Sub"});
    table.declare("Plain", {});
    table.declare("PlainChild", {"Plain"});

    EXPECT_TRUE(table.inherits_builtin("Sub"));
    EXPECT_TRUE(table.inherits_builtin("Deep")) << "transitive";
    EXPECT_FALSE(table.inherits_builtin("Plain"));
    EXPECT_FALSE(table.inherits_builtin("PlainChild"));
}

// object is excluded deliberately: every class conceptually derives from it,
// and is_subtype already returns true for any source against target Object.
// Treating it as "inherits a builtin" would make EVERY attribute miss a
// NotImplementedError and delete the attr-defined check entirely.
TEST(ClassTable, InheritsBuiltinExcludesObject) {
    ClassTable table;
    table.declare("Widget", {"object"});

    EXPECT_FALSE(table.inherits_builtin("Widget"));
}

// A seeded exception class is a Class, not a kind, so it is not a "builtin"
// for this predicate's purposes -- there is no TypeKind::Exception whose
// members we would be unable to model. Its members are simply unknown, which
// is the ordinary user-class case.
TEST(ClassTable, InheritsBuiltinIsFalseForASeededExceptionBase) {
    ClassTable table;
    table.declare("MyError", {"Exception"});

    EXPECT_FALSE(table.inherits_builtin("MyError"));
}

// EnvironmentError, IOError and WindowsError are not distinct classes from
// OSError -- getattr(builtins, "IOError") is builtins.OSError in CPython --
// so both x: IOError = OSError() and y: OSError = IOError() are mypy-clean.
// A bases[] entry cannot express identity, only is-a, so ClassTable
// canonicalises instead.
TEST(ClassTable, CanonicalNameResolvesAnAliasToItsCanonicalSpelling) {
    const ClassTable table;

    EXPECT_EQ(table.canonical_name("IOError"), "OSError");
    EXPECT_EQ(table.canonical_name("OSError"), "OSError");
    EXPECT_EQ(table.canonical_name("Widget"), "Widget");
}

TEST(ClassTable, AliasesShareBasesWithTheirCanonical) {
    const ClassTable table;

    EXPECT_EQ(table.bases_of("IOError"), table.bases_of("OSError"));
}

} // namespace
} // namespace cythonpp::domain::semantic
