#ifndef CYTHONPP_DOMAIN_AST_PARAMETER_H
#define CYTHONPP_DOMAIN_AST_PARAMETER_H

#include <string>
#include <utility>

#include "expr.h"
#include "source_span.h"

namespace cythonpp::domain::ast {

// One parameter in a def: `x`, `x: int`, `x: int = 0`.
//
// A value type rather than a Node, like ComprehensionClause -- a parameter has
// no independent existence and nothing visits one on its own. Both pointers
// may be null: an unannotated parameter, a parameter with no default, or
// neither. It owns unique_ptrs, so it is move-only.
//
// span is a required constructor parameter, span first, matching every Node
// subclass's convention: there is no default and no "unknown" span, so a
// parameter without a real position cannot be built.
struct Parameter {
    Parameter(SourceSpan span, std::string name, ExprPtr annotation, ExprPtr default_value)
        : span(span), name(std::move(name)), annotation(std::move(annotation)),
          default_value(std::move(default_value)) {}

    SourceSpan span;
    std::string name;
    ExprPtr annotation;
    ExprPtr default_value;
};

} // namespace cythonpp::domain::ast

#endif // CYTHONPP_DOMAIN_AST_PARAMETER_H
