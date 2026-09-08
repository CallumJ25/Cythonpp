#ifndef CYTHONPP_DOMAIN_SEMANTIC_TYPE_MAP_H
#define CYTHONPP_DOMAIN_SEMANTIC_TYPE_MAP_H

#include <cstddef>
#include <unordered_map>
#include <utility>

#include "domain/ast/expr.h"
#include "type.h"

namespace cythonpp::domain::semantic {

// Every expression's type, keyed by node address.
//
// Keyed by `const ast::Expr*` and living on CompiledModule, so it cannot
// outlive the Module whose nodes it points into. Nothing else may hold one.
//
// WHAT IS IN IT: every expression ExpressionTyper types.
//
// WHAT IS NOT, both decided deliberately:
//   - Statements. Codegen needs the type of every expression it emits; a
//     statement's type is void or derivable from its parts.
//   - Definitions. A `def` binds a name whose type codegen will want, but
//     that is a SYMBOL TABLE, not an expression map, and this spec cannot
//     know its shape. Guessing produces dead structure codegen works around.
//   - Annotation subtrees. They denote types, not runtime values; codegen
//     reads the resolved Type off the declaration, never off the annotation
//     expression. So `x: int = 5` records an entry for `5` and nothing else.
//
// TypedPrinter appends `:type` only where an entry exists, so annotations
// render bare with no special case -- and a MISSING entry shows up in the
// oracle rather than passing silently.
class TypeMap {
public:
    void insert(const ast::Expr* expr, Type type) { types_[expr] = std::move(type); }

    const Type* find(const ast::Expr* expr) const {
        const auto it = types_.find(expr);
        return it == types_.end() ? nullptr : &it->second;
    }

    bool contains(const ast::Expr* expr) const { return types_.find(expr) != types_.end(); }

    std::size_t size() const { return types_.size(); }

private:
    std::unordered_map<const ast::Expr*, Type> types_;
};

} // namespace cythonpp::domain::semantic

#endif // CYTHONPP_DOMAIN_SEMANTIC_TYPE_MAP_H
