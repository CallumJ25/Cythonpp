#ifndef CYTHONPP_DOMAIN_AST_BREAK_H
#define CYTHONPP_DOMAIN_AST_BREAK_H

#include "stmt.h"
#include "visitor.h"

namespace cythonpp::domain::ast {

// `break`. Carries nothing but its span.
class Break : public Stmt {
public:
    explicit Break(SourceSpan span) : Stmt(span) {}

    void accept(Visitor& visitor) const override { visitor.visit(*this); }
};

} // namespace cythonpp::domain::ast

#endif // CYTHONPP_DOMAIN_AST_BREAK_H
