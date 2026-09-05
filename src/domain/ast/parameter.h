#ifndef CYTHONPP_DOMAIN_AST_PARAMETER_H
#define CYTHONPP_DOMAIN_AST_PARAMETER_H

#include <string>

#include "expr.h"
#include "source_span.h"

namespace cythonpp::domain::ast {

// One parameter in a def: `x`, `x: int`, `x: int = 0`.
//
// A value type rather than a Node, like ComprehensionClause -- a parameter has
// no independent existence and nothing visits one on its own. Both pointers
// may be null: an unannotated parameter, a parameter with no default, or
// neither. It owns unique_ptrs, so it is move-only.
struct Parameter {
    std::string name;
    ExprPtr annotation;
    ExprPtr default_value;
    SourceSpan span;
};

} // namespace cythonpp::domain::ast

#endif // CYTHONPP_DOMAIN_AST_PARAMETER_H
