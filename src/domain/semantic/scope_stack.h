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

    // True ONLY for a method's own first parameter -- the `self` a `def`
    // directly inside a class body binds at index 0. False for every other
    // binding, including a parameter merely SPELLED "self" on a plain
    // function or on a nested def, and including one annotated with the
    // enclosing class.
    //
    // Exists because `self.x = ...` DECLARES an instance attribute exactly
    // when the `self` it stores through is a method's own first parameter,
    // and that question cannot be answered from the binding's TYPE (the only
    // thing TypeChecker::self_attribute_receiver_type used to consult) nor
    // from the innermost function's own method-ness. Measured 2026-09-11
    // against mypy 1.18.1 and CPython 3.14.2, both halves:
    //
    //   - TYPE alone is too LOOSE. A nested `def inner(self: Bag)` inside a
    //     Bag method binds `self` to exactly Class("Bag"), so a type-only
    //     guard passed and `self.q = 1` there declared "q" on Bag -- a later
    //     `self.q` read from another method then came out clean where mypy
    //     reports `"Bag" has no attribute "q"` at BOTH the store and the
    //     read. A missed error.
    //   - "the IMMEDIATELY ENCLOSING function is a method" is too TIGHT, and
    //     wrong in the unsafe direction. mypy attributes a store to the
    //     method's self no matter how many nested function scopes the
    //     reference is closed over: a `def inner()` inside a Bag method that
    //     writes the CAPTURED `self.q = 1` is `Success`, and so is the same
    //     store two closures deep, in both cases with a later `self.q` read
    //     from a different method also clean. Requiring the innermost
    //     function to be a method reported a false TypeError on all of those.
    //
    // What the BINDING gets right and neither of those does: the flag
    // travels with the name, so resolving outward through any number of
    // closures still finds the method's own `self` and still declares, while
    // a nested def's own shadowing parameter is a different binding and does
    // not. Set at the one parameter-binding site in
    // TypeChecker::visit(FunctionDef) from the `is_method` it already
    // computes; nothing else in this codebase binds a method parameter, and
    // no path rebinds `self` (it is order_exempt, so the placeholder-fill
    // rebind cannot mistake it for a placeholder).
    bool method_self = false;

    // The parameter NAMES of `type`, in order, when this binding was made
    // from a `def` statement -- and EMPTY whenever they are not known.
    //
    // Why they live here and not in `Type`: mypy's redefinition rule compares
    // parameter names (measured 2026-09-11, mypy 1.18.1 -- `if c: def g(a:
    // int) -> int` against a prior `def g(b: int) -> int` is `All conditional
    // function variants must have identical signatures  [misc]`, where the
    // same pair with matching names is accepted), but `Type::callable`
    // deliberately carries only {parameter types, return type, defaulted
    // count}. Putting names into `Type` was rejected: `Type` is a copied
    // value compared with an exact `operator==` by unrelated callers, its
    // Callables are also built by builtin_call_table and by
    // ClassTable::constructor_type from places that have no names to supply,
    // and two sites erase `args[0]` to bind `self` -- every one of those
    // would have to learn to keep a parallel name vector in step, and the
    // failure mode of forgetting is a signature whose names and types
    // disagree. A `Binding` is instead exactly as scoped as the question:
    // the rule asks about the binding a redefinition collides with, and
    // `ScopeStack::resolve` already hands that binding over.
    //
    // EMPTY means "not recorded", and the reader must treat it that way
    // rather than as "a zero-parameter signature": only the two `def`-binding
    // sites in TypeChecker fill it, so a binding made by an assignment of a
    // function value (`g = h`) carries h's Callable type with no names at
    // all. TypeChecker::has_identical_signature tells the two apart by
    // LENGTH against the Callable's own parameter count, and falls back to
    // comparing types only when they disagree -- which is the direction that
    // misses an error rather than inventing one. Measured: `g = h` (h taking
    // one parameter) followed by a conditional `def g` with a DIFFERENT
    // parameter name is a mypy error this compiler does not report, while the
    // same shape with a MATCHING name is mypy-clean, so "unknown names means
    // report" would have been a false positive on the second.
    //
    // Declared LAST on purpose: every existing brace-initialisation of a
    // Binding passes one to five members positionally, and appending keeps
    // all of them meaning what they say. Filled by assignment at the two
    // sites that have names, never positionally. (`method_self` was inserted
    // ABOVE this member for the same reason -- appending it below would have
    // pushed `param_names` off the end of the positional prefix.)
    std::vector<std::string> param_names;
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
// resolution. Resolution searches the current scope, then every enclosing
// Function/Comprehension scope, then the Module scope, SKIPPING every
// enclosing Class scope on the way -- whatever kind the current scope is.
//
// The skip is a property of the scope being READ, not of the reader: a class
// body's names are visible only to the code lexically in that same body.
// That covers the familiar case (a method body cannot see its class body's
// names) and the less familiar one it used to get wrong (a class nested in a
// class cannot see the OUTER class body's names either). The current scope is
// resolved before the outward walk begins, so a class body still sees its own
// names.
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
