#ifndef CYTHONPP_DOMAIN_AST_COMPREHENSION_CLAUSE_H
#define CYTHONPP_DOMAIN_AST_COMPREHENSION_CLAUSE_H

#include <vector>

#include "expr.h"

namespace cythonpp::domain::ast {

// One `for target in iterable [if condition]...` clause of a comprehension.
//
// A plain value type rather than a Node: a clause has no independent
// existence in the grammar, and nothing will ever visit one on its own. It
// owns unique_ptrs, so it is move-only.
struct ComprehensionClause {
    ExprPtr target;
    ExprPtr iterable;
    std::vector<ExprPtr> conditions;
};

} // namespace cythonpp::domain::ast

#endif // CYTHONPP_DOMAIN_AST_COMPREHENSION_CLAUSE_H
