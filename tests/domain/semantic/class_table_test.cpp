#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "domain/semantic/class_table.h"
#include "domain/semantic/type.h"
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
    // constructor_type now walks the base chain looking for __init__ too
    // (see ConstructorTypeFindsAnInheritedInit below); this call is what
    // makes a future non-guarded rewrite of that walk hang the test instead
    // of passing silently.
    EXPECT_EQ(type_name(table.constructor_type("A")),
              type_name(Type::callable({}, Type::class_of("A"))));
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

// Verified against mypy 1.18.1: `class B: def __init__(self, a: int) -> None:
// ...` then `class D(B): pass` gives `reveal_type(D)` as
// `def (a: int) -> D` -- D(1) is clean, D() is "Missing positional argument".
// An inherited __init__ IS the subclass's constructor; self is discarded
// either way, so there is no "whose self" question to resolve.
TEST(ClassTable, ConstructorTypeFindsAnInheritedInit) {
    ClassTable table;
    table.declare("B", {});
    table.declare_method(
        "B", "__init__",
        Type::callable({Type::class_of("B"), Type::int_()}, Type::none()));
    table.declare("D", {"B"});

    // The returned instance type is D, the QUERIED class -- not B, the
    // declaring one.
    EXPECT_EQ(type_name(table.constructor_type("D")),
              type_name(Type::callable({Type::int_()}, Type::class_of("D"))));
}

// A subclass's own __init__ shadows the base's, same as member_type's
// "first base wins" -- except here the "first base" is the class itself,
// found before the walk ever looks at bases.
TEST(ClassTable, ConstructorTypeOwnInitBeatsInherited) {
    ClassTable table;
    table.declare("B", {});
    table.declare_method(
        "B", "__init__",
        Type::callable({Type::class_of("B"), Type::int_()}, Type::none()));
    table.declare("D", {"B"});
    table.declare_method(
        "D", "__init__",
        Type::callable({Type::class_of("D"), Type::str()}, Type::none()));

    EXPECT_EQ(type_name(table.constructor_type("D")),
              type_name(Type::callable({Type::str()}, Type::class_of("D"))));
}

TEST(ClassTable, MethodTypeFoundOnTheDeclaringClass) {
    ClassTable table;
    table.declare("Widget", {});
    table.declare_method("Widget", "resize",
                         Type::callable({Type::class_of("Widget"), Type::int_()}, Type::none()));

    const std::optional<Type> resize = table.method_type("Widget", "resize");
    ASSERT_TRUE(resize.has_value());
    EXPECT_EQ(*resize,
              Type::callable({Type::class_of("Widget"), Type::int_()}, Type::none()));

    EXPECT_EQ(table.method_type("Widget", "missing"), std::nullopt);
}

TEST(ClassTable, MethodTypeFoundThroughTheBaseChain) {
    ClassTable table;
    table.declare("Base", {});
    table.declare_method("Base", "greet",
                         Type::callable({Type::class_of("Base")}, Type::str()));
    table.declare("Middle", {"Base"});
    table.declare("Leaf", {"Middle"});

    const std::optional<Type> inherited = table.method_type("Leaf", "greet");
    ASSERT_TRUE(inherited.has_value());
    EXPECT_EQ(*inherited, Type::callable({Type::class_of("Base")}, Type::str()));
}

// Same cycle shape as ACycleInTheBaseChainTerminates -- method_type shares
// walk_chain, so this pins that sharing rather than re-testing the guard
// itself in a second, independent implementation.
TEST(ClassTable, MethodTypeTerminatesOnACycle) {
    ClassTable table;
    table.declare("A", {"B"});
    table.declare("B", {"A"});

    EXPECT_EQ(table.method_type("A", "missing"), std::nullopt);
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

// Every transitive/constructing query canonicalises, not just bases_of --
// pinned individually so a future query that forgets to canonicalise fails
// its own test instead of hiding behind one that happens to still pass.
TEST(ClassTable, IsClassResolvesAnAliasName) {
    const ClassTable table;

    EXPECT_TRUE(table.is_class("IOError"));
}

TEST(ClassTable, MemberTypeResolvesAnAliasName) {
    ClassTable table;
    table.declare_member("OSError", "errno", Type::int_(), 7);

    const std::optional<Type> errno_type = table.member_type("IOError", "errno");
    ASSERT_TRUE(errno_type.has_value());
    EXPECT_EQ(*errno_type, Type::int_());
}

TEST(ClassTable, MethodTypeResolvesAnAliasName) {
    ClassTable table;
    table.declare_method("OSError", "with_traceback",
                         Type::callable({Type::class_of("OSError")}, Type::class_of("OSError")));

    EXPECT_NE(table.method_type("IOError", "with_traceback"), std::nullopt);
}

TEST(ClassTable, ConstructorTypeResolvesAnAliasName) {
    ClassTable table;
    table.declare_method(
        "OSError", "__init__",
        Type::callable({Type::class_of("OSError"), Type::str()}, Type::none()));

    // Note the instance type is OSError, the CANONICAL spelling, even though
    // the query was made under the alias -- constructor_type always returns
    // Class(canonical_name(qualified_name)).
    EXPECT_EQ(type_name(table.constructor_type("IOError")),
              type_name(Type::callable({Type::str()}, Type::class_of("OSError"))));
}

TEST(ClassTable, InheritsBuiltinResolvesAnAliasName) {
    const ClassTable table;

    // OSError's own base chain (Exception) reaches no builtin_type_kind, so
    // this should be false under the alias exactly as it is under OSError
    // itself -- the point is that the alias is canonicalised at all, not
    // which answer comes back.
    EXPECT_EQ(table.inherits_builtin("IOError"), table.inherits_builtin("OSError"));
}

// Shadowing a builtin alias name is legal, ordinary Python:
//   class IOError:
//       pass
//   x: IOError = IOError()
// is mypy --strict clean. declare() writes under the exact spelling with no
// canonicalisation, so a live user entry at "IOError" must win over the
// seeded "OSError" alias target -- otherwise is_class/bases_of/
// constructor_type would all silently answer as OSError instead of the
// user's own (nullary) class, which is invariant-(a)-adjacent but not yet a
// false TypeError on its own (see the next test for that).
TEST(ClassTable, DeclaredClassUnderAnAliasSpellingWinsOverTheBuiltin) {
    ClassTable table;
    table.declare("IOError", {"Widget"});

    EXPECT_TRUE(table.is_class("IOError"));
    EXPECT_EQ(table.bases_of("IOError"), (std::vector<std::string>{"Widget"}));
    EXPECT_EQ(type_name(table.constructor_type("IOError")),
              type_name(Type::callable({}, Type::class_of("IOError"))));
}

// The false-TypeError case named in the finding: without this fix,
// constructor_type("IOError") would resolve __init__ on OSError (nullary),
// so IOError("boom") would draw a false "too many arguments" error, and
// y.tag would draw a false attribute error -- on a program mypy --strict
// accepts.
TEST(ClassTable, DeclaredClassUnderAnAliasSpellingGetsItsOwnConstructor) {
    ClassTable table;
    table.declare("IOError", {});
    table.declare_method(
        "IOError", "__init__",
        Type::callable({Type::class_of("IOError"), Type::str()}, Type::none()));

    EXPECT_EQ(type_name(table.constructor_type("IOError")),
              type_name(Type::callable({Type::str()}, Type::class_of("IOError"))));
}

// The alias fallback must still hold when the user has NOT shadowed the
// name -- this is the regression check that the precedence change (exact
// spelling first) did not disturb the existing alias behaviour for the
// common case where nobody declares a class named "IOError".
TEST(ClassTable, UndeclaredAliasSpellingStillBehavesAsTheCanonicalBuiltin) {
    const ClassTable table;

    EXPECT_EQ(table.canonical_name("IOError"), "OSError");
    EXPECT_TRUE(table.is_class("IOError"));
    EXPECT_EQ(table.bases_of("IOError"), table.bases_of("OSError"));
    EXPECT_EQ(type_name(table.constructor_type("IOError")),
              type_name(Type::callable({}, Type::class_of("OSError"))));
    EXPECT_EQ(table.inherits_builtin("IOError"), table.inherits_builtin("OSError"));
}

// declare_member/declare_method must not fabricate a class entry: a
// mis-spelled or mis-ordered call by the future checking pass should do
// nothing, not silently turn a NameError into a clean annotation.
TEST(ClassTable, DeclareMemberOnAnUndeclaredClassDoesNothing) {
    ClassTable table;
    table.declare_member("Typo", "x", Type::int_(), 1);

    EXPECT_FALSE(table.is_class("Typo"));
    EXPECT_EQ(table.member_type("Typo", "x"), std::nullopt);
}

TEST(ClassTable, DeclareMethodOnAnUndeclaredClassDoesNothing) {
    ClassTable table;
    table.declare_method("Typo", "m", Type::callable({}, Type::none()));

    EXPECT_FALSE(table.is_class("Typo"));
    EXPECT_EQ(table.method_type("Typo", "m"), std::nullopt);
}

// Task 19 fix round 3, Critical 2. A scope-limited alias makes an isolated
// entry reachable under a bare name, and must OUTRANK a live entry under
// that identical spelling: a function-local `class L` shadows a
// module-level `class L` for the rest of that function, exactly as Python's
// own name binding does, so resolving to the module-level one would point a
// member lookup at the wrong class.
TEST(ClassTable, AScopedAliasOutranksALiveEntryUnderTheSameSpelling) {
    ClassTable table;
    table.declare("L", {});
    table.declare_member("L", "a", Type::int_(), 1);
    table.declare("<local-class>#5#L", {});
    table.declare_member("<local-class>#5#L", "b", Type::str(), 6);

    EXPECT_EQ(table.canonical_name("L"), "L");
    EXPECT_EQ(table.member_type("L", "b"), std::nullopt);

    EXPECT_EQ(table.declare_scoped_alias("L", "<local-class>#5#L"), std::nullopt);
    EXPECT_EQ(table.canonical_name("L"), "<local-class>#5#L");
    EXPECT_EQ(table.member_type("L", "b"), Type::str());
    // Task 19 fix round 4: `a` lives on the SHADOWED entry, and the lookup
    // through the alias MISSES it -- but rather than concluding the
    // attribute does not exist, the query falls back to the shadowed entry
    // and finds it. Round 3 asserted nullopt here, which is precisely the
    // false attr-defined this round removes: a `Class("L")` resolved OUTSIDE
    // the function re-canonicalises to the LOCAL class inside it, so
    // "misses through the alias" and "does not exist" are not the same
    // question. See ClassTable::shadowed_name.
    EXPECT_EQ(table.member_type("L", "a"), Type::int_());
    // constructor_type gets NO fallback: it must keep constructing the
    // LOCAL class, or a shadowed __init__'s parameters would make the
    // mypy-clean bare `L()` a false "too few arguments".
    EXPECT_EQ(type_name(table.constructor_type("L")),
              type_name(Type::callable({}, Type::class_of("<local-class>#5#L"))));

    table.remove_scoped_alias("L");
    EXPECT_EQ(table.canonical_name("L"), "L");
    EXPECT_EQ(table.member_type("L", "a"), Type::int_());
    EXPECT_EQ(table.member_type("L", "b"), std::nullopt);
}

// declare_scoped_alias returns the PREVIOUS scoped target so an RAII caller
// can restore rather than erase -- which is what makes a nested function's
// own same-named local class shadow the enclosing one's for exactly its own
// body, and no longer.
TEST(ClassTable, AScopedAliasReportsThePreviousTargetItShadowed) {
    ClassTable table;
    table.declare("<local-class>#2#L", {});
    table.declare("<local-class>#6#L", {});

    EXPECT_EQ(table.declare_scoped_alias("L", "<local-class>#2#L"), std::nullopt);
    EXPECT_EQ(table.declare_scoped_alias("L", "<local-class>#6#L"),
              std::optional<std::string>("<local-class>#2#L"));
    EXPECT_EQ(table.canonical_name("L"), "<local-class>#6#L");

    // What the guard's teardown does with that return value.
    table.declare_scoped_alias("L", "<local-class>#2#L");
    EXPECT_EQ(table.canonical_name("L"), "<local-class>#2#L");
}

// A scoped alias is REMOVABLE; the seeded builtin ones are permanent. The
// two live in separate maps precisely so that difference cannot be blurred
// by accident -- removing a scoped alias must not be able to delete
// IOError -> OSError, and a scoped alias under an alias spelling must
// outrank it while installed.
TEST(ClassTable, AScopedAliasDoesNotDisturbThePermanentBuiltinAliases) {
    ClassTable table;
    table.declare("<local-class>#3#IOError", {});

    table.declare_scoped_alias("IOError", "<local-class>#3#IOError");
    EXPECT_EQ(table.canonical_name("IOError"), "<local-class>#3#IOError");

    table.remove_scoped_alias("IOError");
    EXPECT_EQ(table.canonical_name("IOError"), "OSError");
    EXPECT_TRUE(table.is_class("IOError"));

    // Removing an alias that was never installed is a no-op, not a way to
    // reach into the permanent map.
    table.remove_scoped_alias("EnvironmentError");
    EXPECT_EQ(table.canonical_name("EnvironmentError"), "OSError");
}

// Task 19 fix round 4. The miss-fallback engages ONLY where the scoped alias
// actually shadows something. A function-local class whose name collides
// with no other class shadows nothing, so a genuine attribute miss on it
// stays a miss -- the attr-defined check must still be able to fire.
//
// HONEST LABEL: this is a BOUNDARY PIN, not a defect-detector. It passes
// against the pre-round-4 code by construction (there was no fallback at
// all), and no neutering of shadowed_name breaks it either, because with
// nothing declared under the bare spelling there is nothing for any
// fallback to find. Its value is documenting the intended boundary against
// a FUTURE implementation that widens the fallback (e.g. into a scan of
// every entry). The discriminating tests for this round are
// TypeChecker.AModuleLevelTypedValueKeepsItsOwnClassInsideAShadowingFunction
// and TypeChecker.AShadowedClassesConstructorParametersDoNotReachTheLocalClass.
TEST(ClassTable, AScopedAliasShadowingNothingKeepsAGenuineMemberMissAMiss) {
    ClassTable table;
    table.declare("<local-class>#2#Local", {});
    table.declare_member("<local-class>#2#Local", "x", Type::int_(), 3);
    table.declare_scoped_alias("Local", "<local-class>#2#Local");

    EXPECT_EQ(table.member_type("Local", "x"), Type::int_());
    EXPECT_EQ(table.member_type("Local", "nope"), std::nullopt);
    EXPECT_EQ(table.method_type("Local", "nope"), std::nullopt);
    EXPECT_EQ(table.member_declared_line("Local", "nope"), std::nullopt);
    EXPECT_FALSE(table.inherits_builtin("Local"));
}

// The fallback reaches METHODS and inherits_builtin too, not only plain
// members -- all four are read by the SAME attribute-access arm
// (expression_typer.cpp), so a fallback in one and not the others would
// leave the false attr-defined reachable through whichever was missed.
// inherits_builtin in particular gates the "inherits an unmodelled builtin"
// carve-out, and `true` there only ever SUPPRESSES a diagnostic.
TEST(ClassTable, TheScopedAliasFallbackCoversMethodsAndBuiltinInheritance) {
    ClassTable table;
    table.declare("L", {"list"});
    table.declare_method("L", "b", Type::callable({Type::class_of("L")}, Type::int_()));
    table.declare_member("L", "a", Type::str(), 2);
    table.declare("<local-class>#5#L", {});
    table.declare_scoped_alias("L", "<local-class>#5#L");

    EXPECT_FALSE(table.is_class("nope"));
    EXPECT_EQ(table.method_type("L", "b"),
              Type::callable({Type::class_of("L")}, Type::int_()));
    EXPECT_EQ(table.member_declared_line("L", "a"), 2);
    EXPECT_TRUE(table.inherits_builtin("L"));

    // bases_of is NOT part of the fallback: the isolated entry exists with
    // an empty base list, and that empty answer is legitimate rather than a
    // miss, so it must not be replaced by the shadowed class's bases.
    EXPECT_TRUE(table.bases_of("L").empty());
}

} // namespace
} // namespace cythonpp::domain::semantic
