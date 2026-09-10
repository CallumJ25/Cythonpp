#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "domain/ast/attribute.h"
#include "domain/ast/call.h"
#include "domain/ast/name.h"
#include "domain/ast/source_span.h"
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
// is what makes the `if`-without-`else` case come out right. Verified: mypy
// reveals `builtins.object` there, NOT `int | object`. Object is the top of
// the lattice, so a union containing it IS object; collapsing is an identity,
// not a heuristic, and it makes the answer match the measurement exactly.
TEST(JoinNarrowings, AnUnassignedEdgeContributesTheDeclaredType) {
    const NarrowingState assigned = {{"self.n", Type::int_()}};
    const NarrowingState fallthrough;
    const NarrowingState joined =
        join_narrowings({assigned, fallthrough},
                        [](const NarrowedPath&) -> std::optional<Type> { return Type::object(); });
    ASSERT_EQ(joined.count("self.n"), 1u);
    EXPECT_EQ(joined.at("self.n"), Type::object());
}

// A union containing Object collapses to Object wherever it arises, not only
// in the fall-through case.
TEST(JoinNarrowings, AUnionContainingObjectCollapsesToObject) {
    const NarrowingState left = {{"self.n", Type::object()}};
    const NarrowingState right = {{"self.n", Type::int_()}};
    const NarrowingState joined =
        join_narrowings({left, right},
                        [](const NarrowedPath&) -> std::optional<Type> { return Type::object(); });
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

// An entry EQUAL to the declared type is dropped, keeping the invariant that
// a missing entry means "use the declared type" true in both directions.
TEST(JoinNarrowings, AnEntryEqualToTheDeclaredTypeIsDropped) {
    const NarrowingState left = {{"self.n", Type::object()}};
    const NarrowingState right;
    const NarrowingState joined =
        join_narrowings({left, right},
                        [](const NarrowedPath&) -> std::optional<Type> { return Type::object(); });
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
    const NarrowingState joined =
        join_narrowings({only},
                        [](const NarrowedPath&) -> std::optional<Type> { return Type::object(); });
    EXPECT_EQ(joined.at("self.n"), Type::int_());
}

} // namespace
} // namespace cythonpp::domain::semantic
