#include <gtest/gtest.h>

#include "domain/semantic/type.h"
#include "domain/semantic/type_compatibility.h"
#include "domain/semantic/type_name.h"
#include "fake_class_lookup.h"

namespace cythonpp::domain::semantic {
namespace {

// Named both ways round on failure, and asserted BOTH DIRECTIONS in every
// test: join must be commutative, and mypy's results were symmetric in every
// probe.
void expect_join(const Type& left, const Type& right, const Type& expected,
                 const ClassLookup* classes = nullptr) {
    EXPECT_EQ(type_name(join(left, right, classes)), type_name(expected))
        << "join(" << type_name(left) << ", " << type_name(right) << ")";
    EXPECT_EQ(type_name(join(right, left, classes)), type_name(expected))
        << "join is not commutative for " << type_name(left) << " and " << type_name(right);
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
TEST(Join, CanonicalisesClassNamesBeforeComparing) {
    class AliasingClassLookup : public ClassLookup {
    public:
        bool is_class(const std::string& name) const override {
            return name == "OSError" || name == "IOError" || name == "EnvironmentError" ||
                   name == "WindowsError";
        }
        std::vector<std::string> bases_of(const std::string&) const override { return {}; }
        std::string canonical_name(const std::string& name) const override {
            if (name == "IOError" || name == "EnvironmentError" || name == "WindowsError") {
                return "OSError";
            }
            return name;
        }
    };
    const AliasingClassLookup classes;

    expect_join(Type::class_of("IOError"), Type::class_of("OSError"), Type::class_of("OSError"),
                &classes);
    expect_join(Type::class_of("EnvironmentError"), Type::class_of("WindowsError"),
                Type::class_of("OSError"), &classes);
}

} // namespace
} // namespace cythonpp::domain::semantic
