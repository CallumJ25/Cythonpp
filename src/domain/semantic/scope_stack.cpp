#include "scope_stack.h"

namespace cythonpp::domain::semantic {

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
    scopes_.back().bindings[name] = std::move(binding);
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
