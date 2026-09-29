#include "scope_stack.h"

#include <utility>

namespace cythonpp::domain::semantic {

Binding Binding::placeholder(OrderPosition bound_at, int declared_at) {
    // Type::unknown() is not a choice this factory makes on the caller's
    // behalf -- it is what a placeholder IS. Unknown is absorbing, so a read
    // that resolves to one is answered without a type claim, and the real
    // type arrives from whichever statement later fills it.
    return Binding(Type::unknown(), bound_at, declared_at, /*annotated=*/false,
                   /*order_exempt=*/false, /*method_self=*/false, /*placeholder=*/true);
}

Binding Binding::declared(Type type, OrderPosition bound_at, int declared_at, bool annotated,
                          bool order_exempt, bool method_self) {
    return Binding(std::move(type), bound_at, declared_at, annotated, order_exempt, method_self,
                   /*placeholder=*/false);
}

Binding Binding::fill(Type filled_type, bool annotated, bool order_exempt) const {
    // BOTH positions come from `*this`, the placeholder being replaced, and
    // neither from the filling statement -- see the declaration's comment for
    // why that is invisible today and load-bearing the moment bound_at moves.
    //
    // method_self is false by construction: a method's own `self` is bound
    // directly by visit(FunctionDef)'s parameter loop as a DECLARED binding
    // and never goes through a placeholder, so there is nothing to preserve.
    return Binding(std::move(filled_type), bound_at, declared_at, annotated, order_exempt,
                   /*method_self=*/false, /*placeholder=*/false);
}

ScopeStack::ScopeStack() { scopes_.push_back(Scope{ScopeKind::Module, {}}); }

void ScopeStack::push(ScopeKind kind) { scopes_.push_back(Scope{kind, {}}); }

void ScopeStack::pop() {
    // The module scope (index 0) is permanent: popping it would leave the
    // stack with no current scope at all, which every other method assumes
    // cannot happen. Silently refusing rather than asserting keeps this a
    // pure data structure that never crashes a caller that mis-nests
    // push/pop -- there is no diagnostics sink here to report through
    // anyway.
    if (scopes_.size() > 1) {
        scopes_.pop_back();
    }
}

ScopeKind ScopeStack::current_kind() const { return scopes_.back().kind; }

bool ScopeStack::bind(const std::string& name, Binding binding) {
    return scopes_.back().bindings.emplace(name, std::move(binding)).second;
}

void ScopeStack::rebind(const std::string& name, Binding binding) {
    scopes_.back().bindings.insert_or_assign(name, std::move(binding));
}

Resolution ScopeStack::resolve(const std::string& name) const {
    Resolution result;

    const Scope& current = scopes_.back();
    const auto own = current.bindings.find(name);
    if (own != current.bindings.end()) {
        result.binding = &own->second;
        result.in_own_scope = true;
        return result;
    }

    // EVERY class scope in the outward walk is skipped, whatever kind the
    // reader's own scope is. Python's rule is about the class scope being
    // read, not about who is reading it: a class body is visible only to the
    // code lexically inside that same body, never to anything nested within
    // it. The reader's OWN scope is already handled above, before this loop
    // ever runs, so keying on `current.kind` here bought nothing and was
    // wrong in one direction -- a class nested in a class then saw the outer
    // class body's names, which neither mypy nor CPython allows:
    //
    //   class C1:
    //       x: int = 1
    //       class C2:
    //           y: int = x     # mypy: Name "x" is not defined
    //
    // Verified against mypy 1.18.1 and CPython 3.14: mypy reports
    // name-defined and CPython raises NameError at class-creation time.
    for (std::size_t i = scopes_.size() - 1; i > 0;) {
        --i;
        const Scope& scope = scopes_[i];
        if (scope.kind == ScopeKind::Class) {
            continue;
        }
        const auto found = scope.bindings.find(name);
        if (found != scope.bindings.end()) {
            result.binding = &found->second;
            result.in_own_scope = false;
            return result;
        }
    }

    return result;
}

bool ScopeStack::bound_in_current_scope(const std::string& name) const {
    return scopes_.back().bindings.count(name) > 0;
}

bool ScopeStack::bound_in_an_enclosing_scope(const std::string& name) const {
    // The identical outward walk resolve()'s own fallback performs -- see
    // that function's comment for why every Class scope is skipped -- just
    // run unconditionally, ignoring whatever the current scope holds, rather
    // than only when the current scope misses.
    for (std::size_t i = scopes_.size() - 1; i > 0;) {
        --i;
        const Scope& scope = scopes_[i];
        if (scope.kind == ScopeKind::Class) {
            continue;
        }
        if (scope.bindings.count(name) > 0) {
            return true;
        }
    }
    return false;
}

} // namespace cythonpp::domain::semantic
