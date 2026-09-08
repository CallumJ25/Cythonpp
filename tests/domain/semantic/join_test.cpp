#include <gtest/gtest.h>

#include "domain/semantic/class_table.h"
#include "domain/semantic/type.h"
#include "domain/semantic/type_compatibility.h"
#include "domain/semantic/type_name.h"
#include "fake_class_lookup.h"

namespace cythonpp::domain::semantic {
namespace {

// Named both ways round on failure, and asserted BOTH DIRECTIONS in every
// test: join must be commutative, and mypy's results were symmetric in every
// probe -- EXCEPT the nearest-common-base search under multiple inheritance,
// which is deliberately left-biased to match mypy (see expect_join_directed
// below). Do not use this helper for that case; use the directed one.
void expect_join(const Type& left, const Type& right, const Type& expected,
                 const ClassLookup* classes = nullptr) {
    EXPECT_EQ(type_name(join(left, right, classes)), type_name(expected))
        << "join(" << type_name(left) << ", " << type_name(right) << ")";
    EXPECT_EQ(type_name(join(right, left, classes)), type_name(expected))
        << "join is not commutative for " << type_name(left) << " and " << type_name(right);
}

// For the ONE arm where join is not commutative by design: the nearest-
// common-base search under multiple inheritance. Verified against mypy
// 1.18.1: with `class P`, `class Q`, `class X(P, Q)`, `class Y(Q, P)`,
// `join(X, Y)` is `P` while `join(Y, X)` is `Q` -- mypy itself takes the left
// operand's first base, so the two call orders genuinely disagree. Asserts
// ONLY the given direction; do not "fix" a future failure here by switching
// to expect_join, which would silently paper over a left-bias regression.
void expect_join_directed(const Type& left, const Type& right, const Type& expected,
                          const ClassLookup* classes = nullptr) {
    EXPECT_EQ(type_name(join(left, right, classes)), type_name(expected))
        << "join(" << type_name(left) << ", " << type_name(right) << ")";
}

// Verified: reveal_type([1, 2]) is list[int].
TEST(Join, EquivalentTypesJoinToThemselves) {
    expect_join(Type::int_(), Type::int_(), Type::int_());
    expect_join(Type::str(), Type::str(), Type::str());
    expect_join(Type::list_of(Type::int_()), Type::list_of(Type::int_()),
                Type::list_of(Type::int_()));
}

// is_equivalent, NEVER ==. list[int | str] and list[str | int] are the same
// type despite differing union order; == would send this to object, which is
// the exact shape of Spec 5a's Critical defect.
TEST(Join, EquivalenceIgnoresUnionMemberOrder) {
    expect_join(Type::list_of(Type::union_of({Type::int_(), Type::str()})),
                Type::list_of(Type::union_of({Type::str(), Type::int_()})),
                Type::list_of(Type::union_of({Type::int_(), Type::str()})));
}

// Verified: reveal_type([1, True]) is list[int]; [1, 1.5] is list[float];
// [1.5, 1+2j] is list[complex].
TEST(Join, NumericTypesJoinToTheWiderTowerMember) {
    expect_join(Type::int_(), Type::bool_(), Type::int_());
    expect_join(Type::int_(), Type::float_(), Type::float_());
    expect_join(Type::float_(), Type::complex_(), Type::complex_());
    expect_join(Type::bool_(), Type::complex_(), Type::complex_());
}

// THE EXCEPTION. Verified: reveal_type([1, None]) is list[int | None] -- a
// real union, not object. Everything else joins; None unions.
TEST(Join, NoneUnionsRatherThanJoining) {
    expect_join(Type::int_(), Type::none(), Type::union_of({Type::int_(), Type::none()}));
    expect_join(Type::list_of(Type::int_()), Type::none(),
                Type::union_of({Type::list_of(Type::int_()), Type::none()}));
    // Two Nones are equivalent and collapse before reaching the None arm.
    expect_join(Type::none(), Type::none(), Type::none());
}

// Verified: reveal_type([1, "s"]) is list[object].
TEST(Join, UnrelatedTypesJoinToObject) {
    expect_join(Type::int_(), Type::str(), Type::object());
    expect_join(Type::str(), Type::bytes(), Type::object());
    expect_join(Type::int_(), Type::range_(), Type::object());
}

// Verified: reveal_type([[1], ["a"]]) is list[object] -- NOT
// list[list[object]]. The join is NOT applied recursively into invariant type
// arguments. An implementer who "fixes" this produces a type mypy never
// infers.
TEST(Join, IsNotRecursiveIntoInvariantTypeArguments) {
    expect_join(Type::list_of(Type::int_()), Type::list_of(Type::str()), Type::object());
    expect_join(Type::dict_of(Type::str(), Type::int_()),
                Type::dict_of(Type::str(), Type::str()), Type::object());
}

// Verified: class A, class B(A), class C(A) -- reveal_type([B(), C()]) is
// list[A], and [A(), B()] is list[A].
TEST(Join, ClassesJoinToTheirNearestCommonBase) {
    const semantic_test_support::FakeClassLookup classes(
        {{"A", {}}, {"B", {"A"}}, {"C", {"A"}}});

    expect_join(Type::class_of("B"), Type::class_of("C"), Type::class_of("A"), &classes);
    expect_join(Type::class_of("A"), Type::class_of("B"), Type::class_of("A"), &classes);
    expect_join(Type::class_of("B"), Type::class_of("B"), Type::class_of("B"), &classes);
}

// Verified: reveal_type([B(), 1]) is list[object].
TEST(Join, UnrelatedClassesJoinToObject) {
    const semantic_test_support::FakeClassLookup classes({{"A", {}}, {"Z", {}}});

    expect_join(Type::class_of("A"), Type::class_of("Z"), Type::object(), &classes);
    expect_join(Type::class_of("A"), Type::int_(), Type::object(), &classes);
}

// Verified: `class S(int): pass` -- reveal_type([S(), 1]) is list[int].
TEST(Join, AClassInheritingABuiltinJoinsToThatBuiltin) {
    const semantic_test_support::FakeClassLookup classes({{"S", {"int"}}});

    expect_join(Type::class_of("S"), Type::int_(), Type::int_(), &classes);
    // And through the tower.
    expect_join(Type::class_of("S"), Type::float_(), Type::float_(), &classes);
}

// Without a lookup there is no chain to walk, so unrelated is the only sound
// answer.
TEST(Join, ClassesAreUnrelatedWithoutALookup) {
    expect_join(Type::class_of("B"), Type::class_of("C"), Type::object());
}

// CRITICAL regression: the nearest-common-base search used to walk only the
// LEFT operand's ancestor chain, but is_subtype recognises a supertype that
// appears in NO bases_of chain at all -- the numeric tower. `class S(int)`
// and `class T(float)` share no name in either chain, yet
// is_subtype(Class(S), float) is true, so a left-chain-only search finds
// `object` for `join(S, T)` while finding `float` for `join(T, S)`. Verified
// against mypy 1.18.1: `reveal_type([S(), T()])` and `reveal_type([T(), S()])`
// are BOTH `builtins.list[builtins.float]` -- the numeric tower stays
// commutative even though multiple inheritance (below) does not.
TEST(Join, ClassesInheritingDifferentNumericTowerMembersJoinToTheWiderOne) {
    const semantic_test_support::FakeClassLookup classes({{"S", {"int"}}, {"T", {"float"}}});

    expect_join(Type::class_of("S"), Type::class_of("T"), Type::float_(), &classes);
}

// The nearest-common-base search is deliberately LEFT-BIASED under multiple
// inheritance, matching mypy exactly rather than "fixing" it into a
// symmetric answer. Verified against mypy 1.18.1:
//   class P: pass
//   class Q: pass
//   class X(P, Q): pass
//   class Y(Q, P): pass
//   reveal_type([X(), Y()])  # builtins.list[a.P]
//   reveal_type([Y(), X()])  # builtins.list[a.Q]  -- NOT the same answer.
// join(X, Y) finds P because P is X's own first base (and Y's base chain
// reaches P directly too); join(Y, X) finds Q by the same reasoning with the
// operands swapped. Uses expect_join_directed, NOT expect_join: asserting
// both directions against one `expected` would be wrong here on purpose.
TEST(Join, MultipleInheritanceNearestCommonBaseIsLeftBiasedLikeMypy) {
    const semantic_test_support::FakeClassLookup classes(
        {{"P", {}}, {"Q", {}}, {"X", {"P", "Q"}}, {"Y", {"Q", "P"}}});

    expect_join_directed(Type::class_of("X"), Type::class_of("Y"), Type::class_of("P"), &classes);
    expect_join_directed(Type::class_of("Y"), Type::class_of("X"), Type::class_of("Q"), &classes);
}

// Absorbing, so one root cause draws one diagnostic. An un-annotated
// [x, <error>] must not become list[object] and then fail an unrelated
// assignment check downstream.
TEST(Join, UnknownIsAbsorbing) {
    expect_join(Type::unknown(), Type::int_(), Type::unknown());
    expect_join(Type::unknown(), Type::unknown(), Type::unknown());
    expect_join(Type::unknown(), Type::none(), Type::unknown());
}

TEST(Join, ACycleInTheClassChainTerminates) {
    const semantic_test_support::FakeClassLookup classes({{"A", {"B"}}, {"B", {"A"}}});

    expect_join(Type::class_of("A"), Type::class_of("Z"), Type::object(), &classes);
}

// A recorded direction-(b) gap, pinned so it is a decision rather than a
// surprise: mypy joins ["a", b"b"] to list[Sequence[object]], walking into
// STRUCTURAL supertypes. We produce object. Unreachable as a false positive,
// because Sequence cannot be spelled without imports, so no annotation can
// ever demand it.
TEST(Join, DoesNotWalkIntoStructuralSupertypes) {
    expect_join(Type::str(), Type::bytes(), Type::object());
}

// EnvironmentError, IOError and WindowsError ARE builtins.OSError in CPython
// (all three are aliases assigned to it), so join must canonicalise both
// class names through the lookup before deciding they are unrelated --
// otherwise join(Class("IOError"), Class("OSError")) would wrongly fall to
// Object instead of recognising the two spellings as the very same class.
// ClassLookup grew canonical_name in Task 9, after this brief was written.
//
// Uses the REAL ClassTable, not a hand-rolled fake: fake_class_lookup.h's own
// doc comment states the convention -- FakeClassLookup's canonical_name is
// identity, and a test that needs to exercise actual canonicalisation uses
// ClassTable instead, exactly as type_compatibility_test.cpp already does.
// A hand-rolled alias mapping here would restate ClassTable's own table and
// never notice if the real one diverged.
TEST(Join, CanonicalisesClassNamesBeforeComparing) {
    const ClassTable classes;

    expect_join(Type::class_of("IOError"), Type::class_of("OSError"), Type::class_of("OSError"),
                &classes);
    expect_join(Type::class_of("EnvironmentError"), Type::class_of("WindowsError"),
                Type::class_of("OSError"), &classes);
}

// The canonicalisation above is not top-level only: it recurses into `args`,
// so an aliased class name nested inside a container is resolved too, not
// left as whichever spelling happened to be passed as `left`.
// join(list[IOError], list[OSError]) must return list[OSError], matching the
// bare-class case above rather than reading as a narrower guarantee.
TEST(Join, CanonicalisesClassNamesNestedInsideContainerArguments) {
    const ClassTable classes;

    expect_join(Type::list_of(Type::class_of("IOError")), Type::list_of(Type::class_of("OSError")),
                Type::list_of(Type::class_of("OSError")), &classes);
}

// Regression: operator== compares defaulted_params (see type.h), so two
// Callables differing ONLY in that field are is_equivalent (is_subtype
// ignores it deliberately) and fall into the tie-break arm above. type_less
// must therefore also order by defaulted_params, or the tie-break picks
// whichever operand the caller happened to pass as `left` -- silently
// contradicting this file's own "function of the unordered pair" comment.
// Does not use expect_join/type_name: type_name's Callable rendering never
// shows defaulted_params, so a string comparison would pass even with the
// bug present. Compares the field itself, in both call orders.
TEST(Join, IsOrderIndependentForCallablesDifferingOnlyInDefaultedParams) {
    const Type with_default = Type::callable({Type::int_(), Type::int_()}, Type::int_(), 1);
    const Type without_default = Type::callable({Type::int_(), Type::int_()}, Type::int_(), 0);

    const Type left_first = join(with_default, without_default, nullptr);
    const Type right_first = join(without_default, with_default, nullptr);

    EXPECT_EQ(left_first.defaulted_params, right_first.defaulted_params);
}

} // namespace
} // namespace cythonpp::domain::semantic
