#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "domain/ast/attribute.h"
#include "domain/ast/call.h"
#include "domain/ast/name.h"
#include "domain/ast/source_span.h"
#include "domain/semantic/class_table.h"
#include "domain/semantic/narrowing_map.h"
#include "domain/semantic/type.h"

namespace cythonpp::domain::semantic {
namespace {

ast::SourceSpan span() { return ast::SourceSpan{1, 1, 1, 2}; }

std::unique_ptr<ast::Name> name(std::string identifier) {
    return std::make_unique<ast::Name>(span(), std::move(identifier));
}

std::unique_ptr<ast::Attribute> attribute(ast::ExprPtr value, std::string member) {
    return std::make_unique<ast::Attribute>(span(), std::move(value), std::move(member));
}

// --- narrowing_path_of ----------------------------------------------------

TEST(NarrowingPath, ABareNameIsAPath) {
    EXPECT_EQ(narrowing_path_of(*name("x")), std::optional<NarrowedPath>("x"));
}

// `self` is not special: a plain parameter narrows exactly the same way.
// Verified against mypy 1.18.1, which narrows `b.n` for an ordinary
// parameter `b: Bag` just as it narrows `self.n`.
TEST(NarrowingPath, AnAttributeChainIsADottedPath) {
    EXPECT_EQ(narrowing_path_of(*attribute(name("self"), "n")),
              std::optional<NarrowedPath>("self.n"));
    EXPECT_EQ(narrowing_path_of(*attribute(attribute(name("self"), "inner"), "n")),
              std::optional<NarrowedPath>("self.inner.n"));
    EXPECT_EQ(narrowing_path_of(*attribute(name("b"), "n")),
              std::optional<NarrowedPath>("b.n"));
}

// ANYTHING CONTAINING A CALL IS NOT A PATH and never narrows -- a call may
// return a different object each time, so a syntactic key would be a lie.
TEST(NarrowingPath, AnythingContainingACallIsNotAPath) {
    std::vector<ast::ExprPtr> no_args;
    ast::ExprPtr call = std::make_unique<ast::Call>(span(), name("f"), std::move(no_args));
    EXPECT_FALSE(narrowing_path_of(*call).has_value());
    EXPECT_FALSE(narrowing_path_of(*attribute(std::move(call), "n")).has_value());
}

// --- the map: rules 1, 2 and 3 --------------------------------------------

TEST(NarrowingMap, AMissingEntryIsNullopt) {
    const NarrowingMap map;
    EXPECT_FALSE(map.get("self.n").has_value());
}

TEST(NarrowingMap, SetThenGetReturnsTheNarrowedType) {
    NarrowingMap map;
    map.set("self.n", Type::int_());
    EXPECT_EQ(map.get("self.n"), std::optional<Type>(Type::int_()));
}

TEST(NarrowingMap, SetOverwritesAnExistingEntry) {
    NarrowingMap map;
    map.set("self.n", Type::int_());
    map.set("self.n", Type::str());
    EXPECT_EQ(map.get("self.n"), std::optional<Type>(Type::str()));
}

// KILL ON PREFIX: an assignment to `p` invalidates `p` and every path that
// has `p` as a PROPER dotted prefix. `self.b = B()` invalidates
// `self.b.c.n`, because that object is no longer the one that was narrowed.
TEST(NarrowingMap, KillRemovesThePathAndEveryProperPrefixDescendant) {
    NarrowingMap map;
    map.set("self.b", Type::int_());
    map.set("self.b.c", Type::int_());
    map.set("self.b.c.n", Type::int_());
    map.kill("self.b");
    EXPECT_FALSE(map.get("self.b").has_value());
    EXPECT_FALSE(map.get("self.b.c").has_value());
    EXPECT_FALSE(map.get("self.b.c.n").has_value());
}

// KILL ONLY ON EXACT PREFIXES. Nothing done to one sibling path affects
// another, and a path that merely shares a textual prefix without a dot
// boundary is a DIFFERENT path -- `self.b` must not touch `self.bc`.
TEST(NarrowingMap, KillLeavesSiblingsAndTextualNearMissesAlone) {
    NarrowingMap map;
    map.set("self.b.c", Type::int_());
    map.set("self.d.e", Type::str());
    map.set("self.bc", Type::float_());
    map.kill("self.b");
    EXPECT_FALSE(map.get("self.b.c").has_value());
    EXPECT_EQ(map.get("self.d.e"), std::optional<Type>(Type::str()));
    EXPECT_EQ(map.get("self.bc"), std::optional<Type>(Type::float_()));
}

TEST(NarrowingMap, ClearRemovesEverything) {
    NarrowingMap map;
    map.set("x", Type::int_());
    map.set("self.n", Type::str());
    map.clear();
    EXPECT_FALSE(map.get("x").has_value());
    EXPECT_FALSE(map.get("self.n").has_value());
}

TEST(NarrowingMap, SnapshotAndRestoreRoundTrip) {
    NarrowingMap map;
    map.set("self.n", Type::int_());
    const NarrowingState saved = map.snapshot();
    map.set("self.n", Type::str());
    map.set("other", Type::float_());
    map.restore(saved);
    EXPECT_EQ(map.get("self.n"), std::optional<Type>(Type::int_()));
    EXPECT_FALSE(map.get("other").has_value());
}

// --- rule 4: the join -----------------------------------------------------

// A `declared_type_of` that answers the same type for every path, which is
// all any single-path join test needs.
std::function<std::optional<Type>(const NarrowedPath&)> declaring(Type declared) {
    return [declared](const NarrowedPath&) -> std::optional<Type> { return declared; };
}

// WHAT A JOIN TEST MUST ASSERT: the type a reader ends up with, which is the
// entry when there is one and the declared type otherwise. Where a join
// comes out at the declared type, "entry present holding it" and "entry
// absent" are the same answer to every consumer, and compose the same way
// into a later join -- so a test that pins one of those two representations
// is testing something no caller can observe.
Type effective(const NarrowingState& state, const NarrowedPath& path, const Type& declared) {
    const auto it = state.find(path);
    return it == state.end() ? declared : it->second;
}

// A GENUINE TWO-BRANCH SPLIT GIVES A REAL UNION, not a widen. Verified
// against mypy 1.18.1: `if f: self.n = 7 else: self.n = "s"` reveals
// `builtins.int | builtins.str`.
TEST(JoinNarrowings, TwoAssignedEdgesGiveTheirUnion) {
    const NarrowingState left = {{"self.n", Type::int_()}};
    const NarrowingState right = {{"self.n", Type::str()}};
    const NarrowingState joined =
        join_narrowings({left, right},
                        [](const NarrowedPath&) -> std::optional<Type> { return Type::object(); });
    ASSERT_EQ(joined.count("self.n"), 1u);
    EXPECT_EQ(joined.at("self.n"), Type::union_of({Type::int_(), Type::str()}));
}

// AN EDGE THAT NEVER ASSIGNED THE PATH CONTRIBUTES ITS DECLARED TYPE -- which
// is what makes the `if`-without-`else` case come out right. Verified against
// mypy 1.18.1: with `n: object`, `if f: self.n = 7` then reveals
// `builtins.object`, NOT `int | object`.
TEST(JoinNarrowings, AnUnassignedEdgeContributesTheDeclaredType) {
    const NarrowingState assigned = {{"self.n", Type::int_()}};
    const NarrowingState fallthrough;
    const NarrowingState joined =
        join_narrowings({assigned, fallthrough}, declaring(Type::object()));
    EXPECT_EQ(effective(joined, "self.n", Type::object()), Type::object());
}

// A union containing Object reads as Object wherever it arises, not only in
// the fall-through case: Object is the top of the lattice, so `T | object`
// IS `object`.
TEST(JoinNarrowings, AUnionContainingObjectReadsAsObject) {
    const NarrowingState left = {{"self.n", Type::object()}};
    const NarrowingState right = {{"self.n", Type::int_()}};
    const NarrowingState joined = join_narrowings({left, right}, declaring(Type::object()));
    EXPECT_EQ(effective(joined, "self.n", Type::object()), Type::object());
}

// The Object collapse shapes the value that actually gets STORED. Only
// observable when the declared type is narrower than a contribution, which a
// caller respecting the declared type as a permanent ceiling never produces
// -- the collapse is here so that a value which does get stored is never a
// Union whose Object member would defer every operator on it.
TEST(JoinNarrowings, AStoredUnionContainingObjectIsCollapsed) {
    const NarrowingState left = {{"self.n", Type::object()}};
    const NarrowingState right = {{"self.n", Type::int_()}};
    const NarrowingState joined = join_narrowings({left, right}, declaring(Type::int_()));
    ASSERT_EQ(joined.count("self.n"), 1u);
    EXPECT_EQ(joined.at("self.n"), Type::object());
}

// Agreeing edges collapse, because union_of de-duplicates.
TEST(JoinNarrowings, AgreeingEdgesCollapseToOneType) {
    const NarrowingState left = {{"self.n", Type::int_()}};
    const NarrowingState right = {{"self.n", Type::int_()}};
    const NarrowingState joined =
        join_narrowings({left, right},
                        [](const NarrowedPath&) -> std::optional<Type> { return Type::object(); });
    EXPECT_EQ(joined.at("self.n"), Type::int_());
}

// An entry saying nothing beyond the declared type is dropped. Presence is
// not a promise in the other direction -- see `effective` above -- but a
// join that produced nothing new must not manufacture an entry either.
TEST(JoinNarrowings, AnEntryEqualToTheDeclaredTypeIsDropped) {
    const NarrowingState left = {{"self.n", Type::object()}};
    const NarrowingState right;
    const NarrowingState joined = join_narrowings({left, right}, declaring(Type::object()));
    EXPECT_EQ(joined.count("self.n"), 0u);
}

// A path with NO declared type is dropped rather than guessed at: nothing
// resolvable means nothing to layer a narrowing over.
TEST(JoinNarrowings, APathWithNoDeclaredTypeIsDropped) {
    const NarrowingState left = {{"gone.n", Type::int_()}};
    const NarrowingState right;
    const NarrowingState joined =
        join_narrowings({left, right},
                        [](const NarrowedPath&) -> std::optional<Type> { return std::nullopt; });
    EXPECT_TRUE(joined.empty());
}

TEST(JoinNarrowings, NoEdgesJoinToNothing) {
    EXPECT_TRUE(join_narrowings({}, [](const NarrowedPath&) -> std::optional<Type> {
                    return Type::object();
                }).empty());
}

// A SINGLE edge joins to itself -- the shape a `while` with a body that
// assigns nothing produces, where narrowing must reach past the loop intact.
TEST(JoinNarrowings, ASingleEdgeJoinsToItself) {
    const NarrowingState only = {{"self.n", Type::int_()}};
    const NarrowingState joined = join_narrowings({only}, declaring(Type::object()));
    EXPECT_EQ(joined.at("self.n"), Type::int_());
}

// --- triviality is EQUIVALENCE, not operator== ----------------------------
//
// Every case below joins to something operator== would call different from
// the declared type and store, where mypy 1.18.1 reveals the declared type
// itself. Storing there is not a harmless spelling difference: a Union
// operand defers every operator applied to it, so a program that worked
// before the join stops working after it.

// `n: int` / `if f: self.n = True` reveals `builtins.int`, measured. The raw
// union is `bool | int`, which == would keep.
TEST(JoinNarrowings, ABoolEdgeAgainstADeclaredIntIsTrivial) {
    const NarrowingState narrowed = {{"self.n", Type::bool_()}};
    const NarrowingState fallthrough;
    const NarrowingState joined =
        join_narrowings({narrowed, fallthrough}, declaring(Type::int_()));
    EXPECT_EQ(joined.count("self.n"), 0u);
    EXPECT_EQ(effective(joined, "self.n", Type::int_()), Type::int_());
}

// `n: int` / `if f: self.n = True else: self.n = 3` also reveals
// `builtins.int`, measured -- both edges assign, and it is still trivial.
TEST(JoinNarrowings, ABoolAndAnIntEdgeAgainstADeclaredIntAreTrivial) {
    const NarrowingState left = {{"self.n", Type::bool_()}};
    const NarrowingState right = {{"self.n", Type::int_()}};
    const NarrowingState joined = join_narrowings({left, right}, declaring(Type::int_()));
    EXPECT_EQ(joined.count("self.n"), 0u);
    EXPECT_EQ(effective(joined, "self.n", Type::int_()), Type::int_());
}

// THE NUMERIC TOWER, which is where this matters most in practice.
// `x: float = 0.0` / `if f: x = 3` reveals `builtins.float`, measured, and
// `x + 1.0` afterwards is clean under both oracles (it prints 4.0 then 1.0).
// Storing `int | float` would make that addition a NotImplementedError.
TEST(JoinNarrowings, AnIntEdgeAgainstADeclaredFloatIsTrivial) {
    const NarrowingState narrowed = {{"x", Type::int_()}};
    const NarrowingState fallthrough;
    const NarrowingState joined =
        join_narrowings({narrowed, fallthrough}, declaring(Type::float_()));
    EXPECT_EQ(joined.count("x"), 0u);
    EXPECT_EQ(effective(joined, "x", Type::float_()), Type::float_());
}

// THE ORDER-SENSITIVITY == BROUGHT WITH IT, in both directions. `s: int|str`
// with one branch assigning a str and the other an int reveals
// `builtins.int | builtins.str` whichever branch comes first, measured. ==
// kept one order and dropped the other; equivalence agrees with mypy on both.
TEST(JoinNarrowings, TheDeclaredUnionIsTrivialInEitherBranchOrder) {
    const Type declared = Type::union_of({Type::int_(), Type::str()});
    const NarrowingState str_edge = {{"self.s", Type::str()}};
    const NarrowingState int_edge = {{"self.s", Type::int_()}};

    const NarrowingState str_first = join_narrowings({str_edge, int_edge}, declaring(declared));
    EXPECT_EQ(str_first.count("self.s"), 0u);
    EXPECT_EQ(effective(str_first, "self.s", declared), declared);

    const NarrowingState int_first = join_narrowings({int_edge, str_edge}, declaring(declared));
    EXPECT_EQ(int_first.count("self.s"), 0u);
    EXPECT_EQ(effective(int_first, "self.s", declared), declared);
}

// EQUIVALENCE IS NOT "assignable to": a genuine union still survives against
// a wider declared type. `int | str` is assignable to `object` but object is
// not assignable to it, so the two are not the same type and the narrowing
// is real information.
TEST(JoinNarrowings, AGenuineUnionSurvivesAgainstAWiderDeclaredType) {
    const NarrowingState left = {{"self.n", Type::int_()}};
    const NarrowingState right = {{"self.n", Type::str()}};
    const NarrowingState joined = join_narrowings({left, right}, declaring(Type::object()));
    ASSERT_EQ(joined.count("self.n"), 1u);
    EXPECT_EQ(joined.at("self.n"), Type::union_of({Type::int_(), Type::str()}));
}

// A USER SUBCLASS needs the class table to be recognised as trivial:
// `b: Base` / `if f: self.b = Sub()` reveals `Base`, measured. Without a
// ClassLookup two Class types are simply unrelated, so this is the one case
// where the caller must hand its table over or get a stored union back.
TEST(JoinNarrowings, ASubclassEdgeAgainstItsDeclaredBaseIsTrivial) {
    ClassTable classes;
    classes.declare("Base", {});
    classes.declare("Sub", {Type::class_of("Base")});

    const Type declared = Type::class_of("Base");
    const NarrowingState narrowed = {{"self.b", Type::class_of("Sub")}};
    const NarrowingState fallthrough;
    const NarrowingState joined =
        join_narrowings({narrowed, fallthrough}, declaring(declared), &classes);
    EXPECT_EQ(joined.count("self.b"), 0u);
    EXPECT_EQ(effective(joined, "self.b", declared), declared);
}

// AN UNKNOWN CONTRIBUTION MUST NOT POISON THE JOIN. Type::union_of is
// absorbing on Unknown, and Unknown is compatible with everything in both
// directions, so storing it would silently stop the reader checking a path
// whose declared type was perfectly good. Dropping keeps the declared type.
TEST(JoinNarrowings, AnUnknownContributionDropsTheEntry) {
    const NarrowingState unmodellable = {{"self.n", Type::unknown()}};
    const NarrowingState known = {{"self.n", Type::int_()}};
    const NarrowingState joined = join_narrowings({unmodellable, known}, declaring(Type::int_()));
    EXPECT_EQ(joined.count("self.n"), 0u);
    EXPECT_EQ(effective(joined, "self.n", Type::int_()), Type::int_());
}

} // namespace
} // namespace cythonpp::domain::semantic
