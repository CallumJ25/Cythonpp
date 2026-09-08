#ifndef CYTHONPP_DOMAIN_SEMANTIC_SCOPE_STACK_H
#define CYTHONPP_DOMAIN_SEMANTIC_SCOPE_STACK_H

#include <map>
#include <string>
#include <vector>

#include "type.h"

namespace cythonpp::domain::semantic {

enum class ScopeKind { Module, Class, Function, Comprehension };

// A single name binding: the type it was inferred or declared to have, the
// line it was bound at (for the ordering check a later task performs), and
// whether it came from an explicit annotation.
struct Binding {
    Type type;
    int declared_line = 0;

    // True when the binding came from an explicit annotation. Re-annotating
    // an annotated name is a redefinition error; re-ASSIGNING it is an
    // ordinary assignment check.
    bool annotated = false;

    // General rule: a binding is order-exempt when the name is bound BEFORE
    // the code that may read it, so a same-line read of it can never be a
    // genuine use-before-definition. The ordinary ordering check's `>=`
    // (declared_line >= statement_line_, deliberately `>=` so `x = x + 1`
    // still trips) cannot tell "same line because bound first" apart from
    // "same line because read first" on its own -- this flag is how the
    // binding site, which does know which one it is, tells
    // ExpressionTyper::type_of_name to skip the check entirely.
    //
    // This bug class has shipped three separate times, each caught only
    // after the previous fix had already gone out, because every site looks
    // like an isolated special case until the next one turns up with the
    // same shape:
    //   1. A function parameter -- `def f(x: int) -> None: print(x)` reads
    //      `x` on the `def`'s own line; there is no separate body line for
    //      it to be strictly greater than.
    //   2. A `for` target -- `for i in range(3): print(i)`, same shape.
    //   3. A comprehension target -- `[v * v for v in values]`, same shape
    //      again. This one was found by the labelled corpus AFTER 1013 unit
    //      tests passed, and it fires on essentially every real list
    //      comprehension.
    //
    // The flag also lets assign_to/assign_name tell a parameter apart from
    // pre_bind_function_body's own "still-unfilled placeholder" pattern
    // (same test, declared_line == the current statement's line) -- without
    // it, `def f(x: int) -> None: x = "s"` would be mistaken for the
    // placeholder-fill case and silently REBIND over the parameter's
    // annotation instead of reporting the incompatible assignment.
    //
    // Before adding a fourth site: this is true exactly when the binding is
    // established before the expression(s) that could read it on the same
    // line are typed (a target bound ahead of its RHS/element/body). Leave
    // it false for anything where the read happens first or independently
    // of the bind -- a plain `x = value` RHS, a placeholder rebind, or a
    // signature bound in an outer scope that the body only resolves
    // outward into -- since those are genuine before/after cases where a
    // same-line collision would be a real use-before-definition, not a
    // same-line coincidence.
    bool order_exempt = false;
};

// What a lookup found, and WHERE, because the ordering rule (3b) depends on
// whether the binding lives in the reader's own scope. ScopeStack does not
// decide whether a use-before-definition is an error -- it only reports
// where the binding was found and at what line, and a later task compares.
struct Resolution {
    const Binding* binding = nullptr;   // null when not found
    bool in_own_scope = false;
};

// A pure data structure modelling Python's lexical scoping for name
// resolution. From a Function or Comprehension scope, resolution searches
// self, then enclosing Function/Comprehension scopes SKIPPING every Class
// scope, then the Module scope. A Class scope itself (i.e. when the current
// scope IS a Class, such as evaluating a class body) searches outward
// without skipping -- only method bodies skip class scopes, not the other
// way round.
class ScopeStack {
public:
    ScopeStack();                       // pushes the Module scope

    void push(ScopeKind kind);

    // Pops the current scope. A no-op when only the Module scope remains --
    // the module scope is permanent and popping it is never valid, but this
    // is a pure data structure with no diagnostics sink, so silently
    // refusing rather than asserting is the caller-safe default.
    void pop();

    ScopeKind current_kind() const;

    // Binds in the CURRENT scope. Returns false if the name is already bound
    // there, so the caller can report a redefinition.
    bool bind(const std::string& name, Binding binding);

    // Overwrites an existing binding in the current scope, for the
    // collect-then-check two-pass: the collect pass binds a signature and the
    // check pass must not report a redefinition against itself.
    void rebind(const std::string& name, Binding binding);

    // Resolution order: current scope, then enclosing Function and
    // Comprehension scopes SKIPPING every Class scope, then Module.
    Resolution resolve(const std::string& name) const;

    // True only for the current scope, for the redefinition check.
    bool bound_in_current_scope(const std::string& name) const;

private:
    struct Scope {
        ScopeKind kind;
        std::map<std::string, Binding> bindings;
    };

    std::vector<Scope> scopes_;
};

} // namespace cythonpp::domain::semantic

#endif // CYTHONPP_DOMAIN_SEMANTIC_SCOPE_STACK_H
