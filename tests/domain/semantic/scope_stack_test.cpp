#include <string>

#include <gtest/gtest.h>

#include "domain/semantic/scope_stack.h"
#include "domain/semantic/type.h"

namespace cythonpp::domain::semantic {
namespace {

Binding at(Type type, int line) {
    Binding binding;
    binding.type = std::move(type);
    binding.declared_line = line;
    return binding;
}

TEST(ScopeStack, StartsWithAModuleScope) {
    const ScopeStack scopes;
    EXPECT_EQ(scopes.current_kind(), ScopeKind::Module);
}

TEST(ScopeStack, ResolvesANameBoundInTheCurrentScope) {
    ScopeStack scopes;
    EXPECT_TRUE(scopes.bind("x", at(Type::int_(), 1)));

    const Resolution resolved = scopes.resolve("x");
    ASSERT_NE(resolved.binding, nullptr);
    EXPECT_EQ(resolved.binding->type, Type::int_());
    EXPECT_EQ(resolved.binding->declared_line, 1);
    EXPECT_TRUE(resolved.in_own_scope);
}

TEST(ScopeStack, ReportsAnUnboundName) {
    const ScopeStack scopes;
    const Resolution resolved = scopes.resolve("nope");

    EXPECT_EQ(resolved.binding, nullptr);
    EXPECT_FALSE(resolved.in_own_scope);
}

TEST(ScopeStack, RebindingInTheSameScopeIsRejected) {
    ScopeStack scopes;
    EXPECT_TRUE(scopes.bind("x", at(Type::int_(), 1)));
    EXPECT_FALSE(scopes.bind("x", at(Type::str(), 2))) << "the caller reports a redefinition";

    // The first binding wins, so the reported type stays stable.
    EXPECT_EQ(scopes.resolve("x").binding->type, Type::int_());
}

// The collect pass binds a signature; the check pass must be able to replace
// it without the stack calling it a redefinition against itself.
TEST(ScopeStack, RebindOverwritesWithoutComplaint) {
    ScopeStack scopes;
    scopes.bind("f", at(Type::unknown(), 1));
    scopes.rebind("f", at(Type::callable({}, Type::int_()), 1));

    EXPECT_EQ(scopes.resolve("f").binding->type, Type::callable({}, Type::int_()));
}

// A global read from a function body resolves OUTWARD, which is what makes it
// exempt from the ordering check -- verified: a function body sees a module
// name defined after the def.
TEST(ScopeStack, AFunctionBodySeesModuleGlobalsButNotAsItsOwn) {
    ScopeStack scopes;
    scopes.bind("g", at(Type::int_(), 9));
    scopes.push(ScopeKind::Function);

    const Resolution resolved = scopes.resolve("g");
    ASSERT_NE(resolved.binding, nullptr);
    EXPECT_EQ(resolved.binding->type, Type::int_());
    EXPECT_FALSE(resolved.in_own_scope) << "outward, therefore not order-checked";
}

// Verified: a nested function reads an enclosing function's local and is
// clean.
TEST(ScopeStack, ANestedFunctionReadsEnclosingFunctionLocals) {
    ScopeStack scopes;
    scopes.push(ScopeKind::Function);
    scopes.bind("a", at(Type::int_(), 2));
    scopes.push(ScopeKind::Function);

    const Resolution resolved = scopes.resolve("a");
    ASSERT_NE(resolved.binding, nullptr);
    EXPECT_FALSE(resolved.in_own_scope);
}

// THE RULE WITH TEETH. Verified: `class C:` with `x: int = 1` and
// `def m(self) -> int: return x` reports NameError -- the class body is NOT in
// the method's lexical scope.
TEST(ScopeStack, AMethodBodySkipsTheClassScope) {
    ScopeStack scopes;
    scopes.push(ScopeKind::Class);
    scopes.bind("x", at(Type::int_(), 2));
    scopes.push(ScopeKind::Function);

    EXPECT_EQ(scopes.resolve("x").binding, nullptr) << "class scope must be skipped";
}

// But the method still sees module globals THROUGH the skipped class scope.
TEST(ScopeStack, AMethodBodyStillSeesModuleGlobals) {
    ScopeStack scopes;
    scopes.bind("g", at(Type::str(), 1));
    scopes.push(ScopeKind::Class);
    scopes.push(ScopeKind::Function);

    ASSERT_NE(scopes.resolve("g").binding, nullptr);
    EXPECT_EQ(scopes.resolve("g").binding->type, Type::str());
}

// A class body DOES see enclosing scopes -- only method bodies skip class
// scopes, not the other way round.
TEST(ScopeStack, AClassBodySeesEnclosingScopes) {
    ScopeStack scopes;
    scopes.bind("g", at(Type::int_(), 1));
    scopes.push(ScopeKind::Class);

    ASSERT_NE(scopes.resolve("g").binding, nullptr);
    EXPECT_FALSE(scopes.resolve("g").in_own_scope);
}

// Verified: a comprehension's loop variable does not leak, at module or
// function scope, and a comprehension reads outward freely.
TEST(ScopeStack, AComprehensionScopeIsolatesItsTargetButReadsOutward) {
    ScopeStack scopes;
    scopes.bind("outer", at(Type::int_(), 1));
    scopes.push(ScopeKind::Comprehension);
    scopes.bind("i", at(Type::int_(), 3));

    EXPECT_NE(scopes.resolve("i").binding, nullptr);
    ASSERT_NE(scopes.resolve("outer").binding, nullptr);
    EXPECT_FALSE(scopes.resolve("outer").in_own_scope);

    scopes.pop();
    EXPECT_EQ(scopes.resolve("i").binding, nullptr) << "the loop variable must not leak";
}

// A comprehension nested in a comprehension still reads outward, and the
// intermediate scope is a Comprehension, not a Function -- so the
// class-skipping rule must key on ScopeKind::Class specifically, not on
// "anything that is not a Function".
TEST(ScopeStack, NestedComprehensionsReadOutward) {
    ScopeStack scopes;
    scopes.push(ScopeKind::Comprehension);
    scopes.bind("i", at(Type::int_(), 2));
    scopes.push(ScopeKind::Comprehension);

    ASSERT_NE(scopes.resolve("i").binding, nullptr);
    EXPECT_FALSE(scopes.resolve("i").in_own_scope);
}

TEST(ScopeStack, BoundInCurrentScopeIgnoresEnclosingBindings) {
    ScopeStack scopes;
    scopes.bind("x", at(Type::int_(), 1));
    scopes.push(ScopeKind::Function);

    EXPECT_FALSE(scopes.bound_in_current_scope("x"));
    scopes.bind("x", at(Type::str(), 5));
    EXPECT_TRUE(scopes.bound_in_current_scope("x"));
    // Shadowing, and the inner binding is now "own".
    EXPECT_TRUE(scopes.resolve("x").in_own_scope);
    EXPECT_EQ(scopes.resolve("x").binding->type, Type::str());
}

TEST(ScopeStack, PoppingRestoresTheEnclosingScope) {
    ScopeStack scopes;
    scopes.push(ScopeKind::Function);
    EXPECT_EQ(scopes.current_kind(), ScopeKind::Function);
    scopes.pop();
    EXPECT_EQ(scopes.current_kind(), ScopeKind::Module);
}

TEST(ScopeStack, TheAnnotatedFlagRoundTrips) {
    ScopeStack scopes;
    Binding annotated = at(Type::int_(), 1);
    annotated.annotated = true;
    scopes.bind("x", annotated);

    ASSERT_NE(scopes.resolve("x").binding, nullptr);
    EXPECT_TRUE(scopes.resolve("x").binding->annotated);
}

} // namespace
} // namespace cythonpp::domain::semantic
