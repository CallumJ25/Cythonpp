#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "domain/semantic/builtin_type_names.h"
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

    EXPECT_EQ(table.bases_of("ValueError"), (std::vector<Type>{Type::class_of("Exception")}));
    EXPECT_TRUE(table.bases_of("BaseException").empty());
}

// A seeded base whose name this model represents as a KIND is stored as that
// kind, not as Class("object") or Class("int") -- so the chain walk reaches
// the numeric tower and the container rules for `class Sub(int)`. Verified
// against src/domain/semantic/builtin_class_table.h: "bool" is seeded with
// base "int" (the only seeded builtin-kind-named base in that table), so it
// is the class this pins.
TEST(ClassTable, ASeededBuiltinKindBaseIsStoredAsThatKind) {
    const ClassTable table;
    EXPECT_EQ(table.bases_of("bool"), (std::vector<Type>{Type::int_()}));
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
    table.declare("Button", {Type::class_of("Widget")});

    EXPECT_TRUE(table.is_class("Widget"));
    EXPECT_EQ(table.bases_of("Button"), (std::vector<Type>{Type::class_of("Widget")}));
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
    table.declare("Middle", {Type::class_of("Base")});
    table.declare("Leaf", {Type::class_of("Middle")});

    const std::optional<Type> inherited = table.member_type("Leaf", "x");
    ASSERT_TRUE(inherited.has_value());
    EXPECT_EQ(*inherited, Type::int_());
}

// own_member_type answers the ONE question member_type cannot: is this
// member declared HERE, or inherited? TypeChecker's annotated-self.x branch
// applies two different mypy rules depending on the answer, so a version of
// this that walked the chain (or that just delegated to member_type) would
// collapse them back together and re-introduce the false TypeError the
// distinction exists to remove.
TEST(ClassTable, OwnMemberTypeIgnoresTheBaseChain) {
    ClassTable table;
    table.declare("Base", {});
    table.declare_member("Base", "v", Type::object(), 3);
    table.declare("Child", {Type::class_of("Base")});
    table.declare_member("Child", "w", Type::str(), 7);

    EXPECT_EQ(table.member_type("Child", "v"), Type::object()) << "inherited, as before";
    EXPECT_EQ(table.own_member_type("Child", "v"), std::nullopt)
        << "not declared on Child itself";

    const std::optional<Type> own = table.own_member_type("Child", "w");
    ASSERT_TRUE(own.has_value());
    EXPECT_EQ(*own, Type::str());

    EXPECT_EQ(table.own_member_type("Child", "absent"), std::nullopt);
    EXPECT_EQ(table.own_member_type("NeverDeclared", "v"), std::nullopt);
}

// own_member_declared_line is own_member_type's line half and must miss and
// hit in exactly the same places -- a caller that reads the TYPE off the own
// entry and the LINE off the chain-walking member_declared_line is comparing
// two different classes' entries.
TEST(ClassTable, OwnMemberDeclaredLineIgnoresTheBaseChainToo) {
    ClassTable table;
    table.declare("Base", {});
    table.declare_member("Base", "v", Type::object(), 3);
    table.declare("Child", {Type::class_of("Base")});
    table.declare_member("Child", "w", Type::str(), 7);

    EXPECT_EQ(table.member_declared_line("Child", "v"), 3) << "inherited, as before";
    EXPECT_EQ(table.own_member_declared_line("Child", "v"), std::nullopt)
        << "not declared on Child itself";
    EXPECT_EQ(table.own_member_declared_line("Child", "w"), 7);
    EXPECT_EQ(table.own_member_declared_line("Child", "absent"), std::nullopt);
    EXPECT_EQ(table.own_member_declared_line("NeverDeclared", "v"), std::nullopt);
}

// The third member question: what does this class INHERIT, ignoring its own
// entry? Needed because a class's own entry now exists before any body is
// checked, so "member_type found nothing of mine" no longer separates
// "brand new" from "overriding a base's declaration".
TEST(ClassTable, InheritedMemberTypeSkipsTheClasssOwnEntry) {
    ClassTable table;
    table.declare("Base", {});
    table.declare_member("Base", "v", Type::object(), 3);
    table.declare("Child", {Type::class_of("Base")});
    table.declare_member("Child", "v", Type::int_(), 7);
    table.declare_member("Child", "w", Type::str(), 8);

    EXPECT_EQ(table.member_type("Child", "v"), Type::int_()) << "own entry wins, as before";
    EXPECT_EQ(table.inherited_member_type("Child", "v"), Type::object())
        << "the base's declaration, past Child's own";
    EXPECT_EQ(table.inherited_member_type("Child", "w"), std::nullopt)
        << "declared only on Child itself";
    EXPECT_EQ(table.inherited_member_type("Base", "v"), std::nullopt) << "no bases at all";
    EXPECT_EQ(table.inherited_member_type("NeverDeclared", "v"), std::nullopt);
}

// Multi-level and multiple inheritance, depth-first left to right, matching
// every other base-chain query here.
TEST(ClassTable, InheritedMemberTypeWalksTheWholeChain) {
    ClassTable table;
    table.declare("Root", {});
    table.declare_member("Root", "v", Type::object(), 1);
    table.declare("Mid", {Type::class_of("Root")});
    table.declare("Leaf", {Type::class_of("Mid")});
    table.declare_member("Leaf", "v", Type::bool_(), 9);
    EXPECT_EQ(table.inherited_member_type("Leaf", "v"), Type::object());

    table.declare("Left", {});
    table.declare("Right", {});
    table.declare_member("Right", "w", Type::str(), 2);
    table.declare("Both", {Type::class_of("Left"), Type::class_of("Right")});
    EXPECT_EQ(table.inherited_member_type("Both", "w"), Type::str())
        << "a later base still counts";
}

// THE CYCLE GUARD, and why it has to seed the ROOT. A cyclic base chain
// (`class A(B)` / `class B(A)`, which declare() cannot prevent) walks back
// into the queried class's own entry -- and returning that entry is exactly
// what this query exists NOT to do. mypy rejects a cyclic chain outright, so
// this is unreachable through legal source, but a query whose whole contract
// is "skip my own entry" must not hand it back under any input.
TEST(ClassTable, InheritedMemberTypeDoesNotWalkBackIntoTheQueriedClass) {
    ClassTable table;
    table.declare("A", {Type::class_of("B")});
    table.declare_member("A", "v", Type::int_(), 1);
    table.declare("B", {Type::class_of("A")});
    EXPECT_EQ(table.inherited_member_type("A", "v"), std::nullopt);

    // The self-cycle a function-local `class L(L)` produces, once the local
    // class is declared under its isolated name and the bare name is
    // scope-aliased to it: the base spelling canonicalises straight back to
    // the root.
    table.declare("<local-class>#5#L", {Type::class_of("L")});
    table.declare_member("<local-class>#5#L", "w", Type::str(), 2);
    table.declare_scoped_alias("L", "<local-class>#5#L");
    EXPECT_EQ(table.inherited_member_type("<local-class>#5#L", "w"), std::nullopt);
}

// An alias spelling as a BASE still resolves: a base is a source-level name
// and gets member_type's own canonicalisation, even though the ROOT gets
// own_member_type's (none), because the root is the key the caller is about
// to declare into and a base is not.
TEST(ClassTable, InheritedMemberTypeCanonicalisesABaseSpelling) {
    ClassTable table;
    table.declare_member("OSError", "v", Type::int_(), 1);
    table.declare("Child", {Type::class_of("IOError")});
    table.declare_member("Child", "v", Type::bool_(), 2);
    EXPECT_EQ(table.inherited_member_type("Child", "v"), Type::int_());
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
    table.declare("D", {Type::class_of("A"), Type::class_of("B")});

    const std::optional<Type> resolved = table.member_type("D", "v");
    ASSERT_TRUE(resolved.has_value());
    EXPECT_EQ(*resolved, Type::int_()) << "first base wins";
}

// class A(B) / class B(A) is constructible through declare() and the parser
// cannot reject it. A transitive walk with no visited set infinite-loops, so
// this test is the difference between a hang and a failure.
TEST(ClassTable, ACycleInTheBaseChainTerminates) {
    ClassTable table;
    table.declare("A", {Type::class_of("B")});
    table.declare("B", {Type::class_of("A")});

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
    table.declare("D", {Type::class_of("B")});

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
    table.declare("D", {Type::class_of("B")});
    table.declare_method(
        "D", "__init__",
        Type::callable({Type::class_of("D"), Type::str()}, Type::none()));

    EXPECT_EQ(type_name(table.constructor_type("D")),
              type_name(Type::callable({Type::str()}, Type::class_of("D"))));
}

// A class whose base chain reaches BaseException and which declares no
// __init__ of its own is UNCHECKED. Verified against mypy 1.18.1:
// reveal_type(MyError) for `class MyError(Exception): pass` is
// `def (*args: builtins.object) -> MyError`, and MyError(), MyError("boom")
// and MyError("boom", 42) are all clean. This model has no variadic
// Callable, and inventing one for this alone is not warranted -- so this is
// a CAPABILITY answer ("do not check the arity of this constructor"), not a
// signature. Same reasoning that gave the integer-overflow diagnostic its own
// code rather than folding it into TypeError.
TEST(ClassTable, AnExceptionSubclassWithNoInitIsUnchecked) {
    ClassTable table;
    table.declare("MyError", {Type::class_of("Exception")});
    EXPECT_EQ(table.constructor_check("MyError"), ClassTable::ConstructorCheck::Unchecked);
}

TEST(ClassTable, ASeededExceptionIsUnchecked) {
    const ClassTable table;
    EXPECT_EQ(table.constructor_check("ValueError"), ClassTable::ConstructorCheck::Unchecked);
    EXPECT_EQ(table.constructor_check("OSError"), ClassTable::ConstructorCheck::Unchecked);
    // Reached through the alias spelling too, like every other read query.
    EXPECT_EQ(table.constructor_check("IOError"), ClassTable::ConstructorCheck::Unchecked);
}

// An ordinary class is checked. Verified against mypy 1.18.1: `Plain("x")`
// really is `Too many arguments for "Plain"  [call-arg]`, and an explicit
// `object` base produces byte-identical mypy output (diffed; CPython says
// `TypeError: Plain() takes no arguments` for both).
TEST(ClassTable, APlainClassIsChecked) {
    ClassTable table;
    table.declare("Plain", {});
    EXPECT_EQ(table.constructor_check("Plain"), ClassTable::ConstructorCheck::Checked);
}

// A DECLARED __init__ wins: an exception subclass that defines its own
// constructor is checked against it.
TEST(ClassTable, AnExceptionSubclassWithItsOwnInitIsCheckedNormally) {
    ClassTable table;
    table.declare("MyError", {Type::class_of("Exception")});
    table.declare_method("MyError", "__init__",
                         Type::callable({Type::class_of("MyError"), Type::str()}, Type::none()));
    EXPECT_EQ(table.constructor_check("MyError"), ClassTable::ConstructorCheck::Checked);
    EXPECT_EQ(type_name(table.constructor_type("MyError")), "Callable[[str], MyError]");
}

// __new__ WITH NO __init__ is unchecked too. Measured against mypy 1.18.1:
// __new__ participates fully when no __init__ exists (a class declaring only
// `__new__(cls, a: int)` reveals as `def (a: builtins.int) -> N`), and loses
// outright to __init__ when both exist (a class declaring both
// `__new__(cls, a: int)` and `__init__(self, b: str)` reveals as
// `def (b: builtins.str) -> M`, with no complaint about the contradiction).
// Modelling it properly is out of scope, so the fallback deliberately errs
// toward a MISSED error rather than a false "too few arguments" on every
// construction.
TEST(ClassTable, AClassDeclaringOnlyNewIsUnchecked) {
    ClassTable table;
    table.declare("N", {});
    table.declare_method("N", "__new__",
                         Type::callable({Type::class_of("type"), Type::int_()},
                                        Type::class_of("N")));
    EXPECT_EQ(table.constructor_check("N"), ClassTable::ConstructorCheck::Unchecked);
}

// A class whose base chain reaches a builtin KIND (int, str, list, ...) is
// UNMODELLABLE, not unchecked -- the two answers differ because the
// constructors do. Measured against mypy 1.18.1 and CPython:
// `class MyInt(int): pass` makes MyInt(3) and MyInt("ff", 16) `Success` /
// clean runs, but MyInt(1, 2, 3) is `No overload variant of "MyInt" matches
// argument types "int", "int", "int"  [call-overload]` and CPython raises
// `TypeError: int() takes at most 2 arguments (3 given)`. A bounded overload
// set, unlike BaseException's genuine `*args: object` -- so silence would be
// silent acceptance of a program both oracles reject, and the caller reports
// NotImplementedError instead.
TEST(ClassTable, AClassInheritingABuiltinKindWithNoInitIsUnmodellable) {
    ClassTable table;
    table.declare("MyInt", {Type::int_()});
    EXPECT_EQ(table.constructor_check("MyInt"), ClassTable::ConstructorCheck::Unmodellable);

    ClassTable table2;
    table2.declare("C", {Type::str()});
    EXPECT_EQ(table2.constructor_check("C"), ClassTable::ConstructorCheck::Unmodellable);
}

// The search is POSITION-AWARE, because Python and mypy resolve the
// constructor by MRO: a builtin base to the LEFT of a plain base that
// declares __init__ wins, and the plain base to the left wins right back.
// Verified against mypy 1.18.1, with `class Mixin: def __init__(self, a: str)`:
// `class MyInt(int, Mixin): pass` then MyInt(3) is `Success` (and CPython
// prints 3), while `class MyInt(Mixin, int): pass` then MyInt(3) is
// `Argument 1 to "MyInt" has incompatible type "int"; expected "str"
// [arg-type]`.
TEST(ClassTable, ABuiltinBaseLeftOfADeclaredInitWins) {
    ClassTable table;
    table.declare("Mixin", {});
    table.declare_method("Mixin", "__init__",
                         Type::callable({Type::class_of("Mixin"), Type::str()}, Type::none()));
    table.declare("MyInt", {Type::int_(), Type::class_of("Mixin")});
    EXPECT_EQ(table.constructor_check("MyInt"), ClassTable::ConstructorCheck::Unmodellable);
}

// PINS the precedence fix directly: the __new__ fallback is whole-chain, so
// if it ran BEFORE the builtin-base decision it would find Mixin's __new__
// (declared here alongside __init__) and answer Unchecked -- silence -- on a
// call neither oracle accepts. Distinct from ABuiltinBaseLeftOfADeclaredInitWins
// above, whose Mixin declares only __init__ and so never exercised the
// __new__ walk at all. Verified against mypy 1.18.1 and CPython with
// `class Marker: pass`, `class Mixin: def __new__(cls, a: int) -> Marker:
// ...` / `def __init__(self, a: str) -> None: ...`, and
// `class MyInt(int, Mixin): pass`: `MyInt(1, 2, 3)` is `No overload variant
// of "MyInt" matches argument types "int", "int", "int"  [call-overload]`
// under mypy and `TypeError: int() takes at most 2 arguments (3 given)`
// under CPython -- both oracles reject it, so this must stay Unmodellable.
TEST(ClassTable, ABuiltinBaseLeftOfAMixinDeclaringBothNewAndInitStaysUnmodellable) {
    ClassTable table;
    table.declare("Marker", {});
    table.declare("Mixin", {});
    table.declare_method(
        "Mixin", "__new__",
        Type::callable({Type::class_of("Mixin"), Type::int_()}, Type::class_of("Marker")));
    table.declare_method("Mixin", "__init__",
                         Type::callable({Type::class_of("Mixin"), Type::str()}, Type::none()));
    table.declare("MyInt", {Type::int_(), Type::class_of("Mixin")});
    EXPECT_EQ(table.constructor_check("MyInt"), ClassTable::ConstructorCheck::Unmodellable);
}

TEST(ClassTable, ADeclaredInitLeftOfABuiltinBaseWins) {
    ClassTable table;
    table.declare("Mixin", {});
    table.declare_method("Mixin", "__init__",
                         Type::callable({Type::class_of("Mixin"), Type::str()}, Type::none()));
    table.declare("MyInt", {Type::class_of("Mixin"), Type::int_()});
    EXPECT_EQ(table.constructor_check("MyInt"), ClassTable::ConstructorCheck::Checked);
    EXPECT_EQ(type_name(table.constructor_type("MyInt")), "Callable[[str], MyInt]");
}

// ---------------------------------------------------------------------------
// A seeded builtin row's own constructor arity band (Entry::builtin_arity)
// must NEVER hijack the DeclaredInit/BuiltinKindBase/__new__ arms for a
// SUBCLASS whose base chain merely reaches that row. An earlier version of
// this seeding wrote the band into `methods["__init__"]` instead of its own
// field, which made the walk's `methods.find("__init__")` check hit at the
// builtin row and answer DeclaredInit -- before the BuiltinKindBase arm and
// before the whole-chain __new__ fallback, both of which must still win.
// Measured 2026-09-12 against mypy 1.18.1 and CPython:
//   class Singleton(object):
//       def __new__(cls, tag: int) -> "Singleton": ...
//   Singleton(7)
// is mypy Success and CPython prints the object cleanly -- the synthetic-
// __init__ version reported a false `too many arguments for "Singleton"` by
// hijacking DeclaredInit via `object`'s own seeded (0, 0) row.
TEST(ClassTable, ANewOverAnObjectBaseWithNoInitIsUncheckedNotHijacked) {
    ClassTable table;
    table.declare("Singleton", {Type::object()});
    table.declare_method(
        "Singleton", "__new__",
        Type::callable({Type::class_of("Singleton"), Type::int_()}, Type::class_of("Singleton")));
    EXPECT_EQ(table.constructor_check("Singleton"), ClassTable::ConstructorCheck::Unchecked);
}

// The same hijack, but over a MODEL-KIND base (`float`), where the correct
// answer without the hijack is Unmodellable (a bounded overload set this
// model cannot spell), not Checked and not Unchecked. Measured: `class
// Pair(float): def __new__(cls, a: int, b: int) -> "Pair": ...` then
// `Pair(1, 2)` is mypy Success and CPython-clean; the synthetic-__init__
// version reported a false `too many arguments for "Pair"` by hijacking
// DeclaredInit via `float`'s own seeded (0, 1) row, before ever reaching the
// BuiltinKindBase arm that `float` (a model kind) would otherwise take.
TEST(ClassTable, ANewOverAFloatBaseWithNoInitStaysUnmodellableNotHijacked) {
    ClassTable table;
    table.declare("Pair", {Type::float_()});
    table.declare_method(
        "Pair", "__new__",
        Type::callable({Type::class_of("Pair"), Type::int_(), Type::int_()}, Type::class_of("Pair")));
    EXPECT_EQ(table.constructor_check("Pair"), ClassTable::ConstructorCheck::Unmodellable);
}

// The same hijack over `tuple`, the other model-kind bounded row (seeded
// (0, 1)). Measured: `class Point(tuple): def __new__(cls, a: int, b: int)
// -> "Point": ...` then `Point(1, 2)` is mypy Success and CPython-clean.
TEST(ClassTable, ANewOverATupleBaseWithNoInitStaysUnmodellableNotHijacked) {
    ClassTable table;
    table.declare("Point", {Type::tuple_of({})});
    table.declare_method("Point", "__new__",
                         Type::callable({Type::class_of("Point"), Type::int_(), Type::int_()},
                                        Type::class_of("Point")));
    EXPECT_EQ(table.constructor_check("Point"), ClassTable::ConstructorCheck::Unmodellable);
}

// ---------------------------------------------------------------------------
// declare() must reset a shadowed builtin row's arity band, exactly as it
// already resets `bases` (see declare()'s own comment) -- a real `class`
// statement reusing a builtin spelling is legal Python and gets a genuinely
// FRESH entry, not the builtin's leftover state.
// ---------------------------------------------------------------------------

// FALSE POSITIVE if unfixed: `class memoryview: pass` / `memoryview()` is
// mypy Success and CPython-clean, but inheriting the seeded (1, 1) band
// reports a false `too few arguments for "memoryview"`.
TEST(ClassTable, DeclaringOverABoundedBuiltinNameGetsAFreshZeroArgConstructor) {
    ClassTable table;
    table.declare("memoryview", {});
    EXPECT_EQ(table.constructor_check("memoryview"), ClassTable::ConstructorCheck::Checked);
    EXPECT_EQ(type_name(table.constructor_type("memoryview")), "Callable[[], memoryview]");
}

// MISSED ERROR if unfixed: `class slice: pass` / `slice(1, 2, 3)` -- mypy
// `Too many arguments`, CPython `TypeError: slice() takes no arguments` --
// both oracles reject it, but inheriting the seeded unbounded band routes
// the shadowing class through Unchecked instead of Checked, going silent.
TEST(ClassTable, DeclaringOverAnUnboundedBuiltinNameIsCheckedNotUnchecked) {
    ClassTable table;
    table.declare("slice", {});
    EXPECT_EQ(table.constructor_check("slice"), ClassTable::ConstructorCheck::Checked);
    EXPECT_EQ(type_name(table.constructor_type("slice")), "Callable[[], slice]");
}

// ---------------------------------------------------------------------------
// A subclass with no `__init__`/`__new__` of its own must INHERIT its
// nearest builtin ancestor's arity band, not fall to the ordinary zero-arg
// default. An earlier version of this fix consulted only the RESOLVED
// class's own entry (to avoid the DeclaredInit-hijack Critical 1 named), and
// that also removed this inheritance -- so a subclass of a bounded row was
// newly broken by the very fix that closed the hijack. Both cases below
// cover a GRANDCHILD too, two plain classes down from the seeded row.
// ---------------------------------------------------------------------------

// FALSE POSITIVE if unfixed: `class cached(property): pass` used as
// `cached(getter)` (one argument) is mypy Success and CPython-clean.
// Three-way history: `8fa41a8` (pre-feature) reported "too many arguments";
// `9f1f3ca` (the synthetic-__init__ hijack) was accidentally silent, since
// `property`'s hijacked entry was found by the OLD chain-based walk;
// `69e2b57` (resolved-entry-only) reported again, for a different reason --
// `cached` itself has no band, and the walk no longer reached `property`'s.
TEST(ClassTable, ASubclassOfABoundedNonKindRowInheritsItsBand) {
    ClassTable table;
    table.declare("cached", {Type::class_of("property")});
    EXPECT_EQ(table.constructor_check("cached"), ClassTable::ConstructorCheck::Checked);
    EXPECT_EQ(type_name(table.constructor_type("cached")),
              "Callable[[Unknown, Unknown, Unknown, Unknown], cached]");

    // Grandchild: two plain classes between the query and the seeded row.
    table.declare("Mid", {Type::class_of("cached")});
    table.declare("Grandchild", {Type::class_of("Mid")});
    EXPECT_EQ(table.constructor_check("Grandchild"), ClassTable::ConstructorCheck::Checked);
    EXPECT_EQ(type_name(table.constructor_type("Grandchild")),
              "Callable[[Unknown, Unknown, Unknown, Unknown], Grandchild]");
}

// MISSED ERROR if unfixed: `class MyU(UnicodeDecodeError): pass` then
// `MyU(1)` -- mypy reports (incl.) `Too few arguments for "MyU"`, CPython
// `TypeError: function takes exactly 5 arguments (1 given)`, both oracles
// reject it. `9f1f3ca` reported the right thing here (the hijacked entry
// was found via the chain walk too); the resolved-entry-only version of
// this fix (`69e2b57`) went SILENT, a genuine regression inside this range,
// since `MyU` reaches `BaseExceptionBase` through its own base chain and its
// own entry (not `UnicodeDecodeError`'s) is what a resolved-entry-only
// lookup would have consulted.
TEST(ClassTable, ASubclassOfABoundedExceptionRowInheritsItsBand) {
    ClassTable table;
    table.declare("MyU", {Type::class_of("UnicodeDecodeError")});
    EXPECT_EQ(table.constructor_check("MyU"), ClassTable::ConstructorCheck::Checked);
    EXPECT_EQ(type_name(table.constructor_type("MyU")),
              "Callable[[Unknown, Unknown, Unknown, Unknown, Unknown], MyU]");

    // Grandchild, same shape as the property case above.
    table.declare("Mid2", {Type::class_of("MyU")});
    table.declare("Grandchild2", {Type::class_of("Mid2")});
    EXPECT_EQ(table.constructor_check("Grandchild2"), ClassTable::ConstructorCheck::Checked);
    EXPECT_EQ(type_name(table.constructor_type("Grandchild2")),
              "Callable[[Unknown, Unknown, Unknown, Unknown, Unknown], Grandchild2]");
}

// ---------------------------------------------------------------------------
// `object` is itself a SEEDED row and carries a BOUNDED band, (0, 0) -- not
// merely "no band" the way an ordinary user class is -- so
// find_builtin_arity's first-branch-wins walk can hand `object`'s band to a
// completely unrelated chain that merely happens to pass through it as
// SOMEONE ELSE's implicit ancestor. Measured 2026-09-13: `class M(object):
// pass` / `class C(M, Exception): pass` then `C("boom")` is mypy-clean and
// CPython-clean (prints "boom"), but the walk found `object`'s (0, 0) via
// `M` before ever reaching `Exception`'s own unbounded band, flipping `C`'s
// genuinely variadic BaseException constructor to Checked against a
// zero-arg signature. Closed the same way `constructor_check`'s own
// positional walk and `inherits_builtin` already exclude `object` as an
// ancestor: it answers for the class actually asked about, never for one
// reached only by walking through it.
// ---------------------------------------------------------------------------

TEST(ClassTable, AnObjectBaseReachedThroughAnUnrelatedAncestorDoesNotHijackAnExceptionBase) {
    ClassTable table;
    table.declare("M", {Type::object()});
    table.declare("C", {Type::class_of("M"), Type::class_of("Exception")});
    EXPECT_EQ(table.constructor_check("C"), ClassTable::ConstructorCheck::Unchecked);
}

// ORDER-SENSITIVE, the first-branch-wins tie-break showing its teeth: with
// the bases swapped, `Exception`'s own (unbounded) band is found FIRST
// regardless of the `object` carve-out, so this direction was already
// correct -- pinned as a control so a future change to the tie-break cannot
// silently reintroduce the hijack from this side without a test noticing.
TEST(ClassTable, TheSameProgramWithBasesSwappedStaysUnchecked) {
    ClassTable table;
    table.declare("M", {Type::object()});
    table.declare("C", {Type::class_of("Exception"), Type::class_of("M")});
    EXPECT_EQ(table.constructor_check("C"), ClassTable::ConstructorCheck::Unchecked);
}

// CONTROL: `object` constructed DIRECTLY must keep reporting -- the
// carve-out excludes `object` only when it is reached as an ANCESTOR of a
// different resolved class, never when it is the class actually being asked
// about. Mirrors TypeChecker.AZeroArgBoundedBuiltinStillReportsTooManyArguments
// one layer down, at the ClassTable level the carve-out itself lives at.
TEST(ClassTable, ObjectConstructedDirectlyStaysBoundedAtZeroArgs) {
    const ClassTable table;
    EXPECT_EQ(table.constructor_check("object"), ClassTable::ConstructorCheck::Checked);
    EXPECT_EQ(type_name(table.constructor_type("object")), "Callable[[], object]");
}

// The same rule for an exception base. Verified against mypy 1.18.1 with the
// same Mixin: `class MyErr(Exception, Mixin): pass` then MyErr("boom", 42) is
// `Success` and CPython prints `('boom', 42)`, where the whole-chain search
// reported a false `too many arguments for "MyErr"`.
TEST(ClassTable, AnExceptionBaseLeftOfADeclaredInitWins) {
    ClassTable table;
    table.declare("Mixin", {});
    table.declare_method("Mixin", "__init__",
                         Type::callable({Type::class_of("Mixin"), Type::str()}, Type::none()));
    table.declare("MyErr", {Type::class_of("Exception"), Type::class_of("Mixin")});
    EXPECT_EQ(table.constructor_check("MyErr"), ClassTable::ConstructorCheck::Unchecked);
}

// Position is about which base is reached FIRST, not about depth: a declared
// __init__ two levels up the LEFTMOST base still beats an exception base to
// its right. Verified against mypy 1.18.1: with `class Grand` declaring
// `__init__(self, a: int)`, `class Mid(Grand)` and `class Leaf(Mid,
// Exception)`, `Leaf("s")` is `Argument 1 to "Leaf" has incompatible type
// "str"; expected "int"  [arg-type]`.
TEST(ClassTable, ADeclaredInitTwoLevelsUpTheLeftBaseBeatsAnExceptionBase) {
    ClassTable table;
    table.declare("Grand", {});
    table.declare_method("Grand", "__init__",
                         Type::callable({Type::class_of("Grand"), Type::int_()}, Type::none()));
    table.declare("Mid", {Type::class_of("Grand")});
    table.declare("Leaf", {Type::class_of("Mid"), Type::class_of("Exception")});
    EXPECT_EQ(table.constructor_check("Leaf"), ClassTable::ConstructorCheck::Checked);
    EXPECT_EQ(type_name(table.constructor_type("Leaf")), "Callable[[int], Leaf]");
}

// REGRESSION GUARDS for behaviour that is already correct and was untested:
// with multiple inheritance where only the SECOND base declares __init__,
// that one is used -- an implicit object.__init__ does not shadow it, so the
// walk must look for a class that DECLARES __init__, not one whose __init__
// merely resolves. Verified against mypy 1.18.1: reveal_type(C) is
// `def (n: builtins.int) -> C`.
TEST(ClassTable, ConstructorResolutionFindsASecondBasesInit) {
    ClassTable table;
    table.declare("A", {});
    table.declare("B", {});
    table.declare_method("B", "__init__",
                         Type::callable({Type::class_of("B"), Type::int_()}, Type::none()));
    table.declare("C", {Type::class_of("A"), Type::class_of("B")});
    EXPECT_EQ(type_name(table.constructor_type("C")), "Callable[[int], C]");
}

// Where two bases BOTH declare one, the LEFT base wins, and mypy reports no
// LSP complaint. Verified against mypy 1.18.1.
TEST(ClassTable, ConstructorResolutionPrefersTheLeftBasesInit) {
    ClassTable table;
    table.declare("A", {});
    table.declare_method("A", "__init__",
                         Type::callable({Type::class_of("A"), Type::str()}, Type::none()));
    table.declare("B", {});
    table.declare_method("B", "__init__",
                         Type::callable({Type::class_of("B"), Type::int_()}, Type::none()));
    table.declare("C", {Type::class_of("A"), Type::class_of("B")});
    EXPECT_EQ(type_name(table.constructor_type("C")), "Callable[[str], C]");
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
    table.declare("Middle", {Type::class_of("Base")});
    table.declare("Leaf", {Type::class_of("Middle")});

    const std::optional<Type> inherited = table.method_type("Leaf", "greet");
    ASSERT_TRUE(inherited.has_value());
    EXPECT_EQ(*inherited, Type::callable({Type::class_of("Base")}, Type::str()));
}

// Same cycle shape as ACycleInTheBaseChainTerminates -- method_type shares
// walk_chain, so this pins that sharing rather than re-testing the guard
// itself in a second, independent implementation.
TEST(ClassTable, MethodTypeTerminatesOnACycle) {
    ClassTable table;
    table.declare("A", {Type::class_of("B")});
    table.declare("B", {Type::class_of("A")});

    EXPECT_EQ(table.method_type("A", "missing"), std::nullopt);
}

// The predicate that forces Task 20's attribute carve-out. Verified:
// `class Sub(int): pass` then Sub().bit_length() is mypy-CLEAN, so an
// attribute miss on Sub must not be a TypeError.
TEST(ClassTable, InheritsBuiltinDetectsABuiltinInTheBaseChain) {
    ClassTable table;
    table.declare("Sub", {Type::int_()});
    table.declare("Deep", {Type::class_of("Sub")});
    table.declare("Plain", {});
    table.declare("PlainChild", {Type::class_of("Plain")});

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
    table.declare("Widget", {Type::object()});

    EXPECT_FALSE(table.inherits_builtin("Widget"));
}

// A seeded exception class is a Class, not a kind, so it is not a "builtin"
// for this predicate's purposes -- there is no TypeKind::Exception whose
// members we would be unable to model. Its members are simply unknown, which
// is the ordinary user-class case.
TEST(ClassTable, InheritsBuiltinIsFalseForASeededExceptionBase) {
    ClassTable table;
    table.declare("MyError", {Type::class_of("Exception")});

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
    table.declare("IOError", {Type::class_of("Widget")});

    EXPECT_TRUE(table.is_class("IOError"));
    EXPECT_EQ(table.bases_of("IOError"), (std::vector<Type>{Type::class_of("Widget")}));
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

// A scope-limited alias makes an isolated
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
    // `a` lives on the SHADOWED entry, and the lookup
    // through the alias MISSES it -- but rather than concluding the
    // attribute does not exist, the query falls back to the shadowed entry
    // and finds it. An earlier version asserted nullopt here, which is
    // precisely the false attr-defined the fallback removes: a `Class("L")`
    // resolved OUTSIDE
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

// The miss-fallback engages ONLY where the scoped alias
// actually shadows something. A function-local class whose name collides
// with no other class shadows nothing, so a genuine attribute miss on it
// stays a miss -- the attr-defined check must still be able to fire.
//
// HONEST LABEL: this is a BOUNDARY PIN, not a defect-detector. It passes
// against the code that predates the fallback by construction (there was
// none at all), and no neutering of shadowed_name breaks it either, because
// with
// nothing declared under the bare spelling there is nothing for any
// fallback to find. Its value is documenting the intended boundary against
// a FUTURE implementation that widens the fallback (e.g. into a scan of
// every entry). The discriminating tests for the fallback are
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
    table.declare("L", {builtin_base_type(TypeKind::List)});
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
