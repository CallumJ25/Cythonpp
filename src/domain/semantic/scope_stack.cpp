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

    // Class-scope skipping only applies when the search originates from a
    // Function or Comprehension: those are the scopes whose bodies genuinely
    // cannot see an enclosing class body's names. A Class scope searching
    // outward (evaluating a class body) does not skip anything -- only
    // method bodies skip class scopes, not the other way round.
    const bool skip_class_scopes =
        current.kind == ScopeKind::Function || current.kind == ScopeKind::Comprehension;

    for (std::size_t i = scopes_.size() - 1; i > 0;) {
        --i;
        const Scope& scope = scopes_[i];
        if (skip_class_scopes && scope.kind == ScopeKind::Class) {
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

} // namespace cythonpp::domain::semantic
