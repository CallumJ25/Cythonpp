#ifndef CYTHONPP_DOMAIN_AST_CONTINUE_H
#define CYTHONPP_DOMAIN_AST_CONTINUE_H

#include "stmt.h"
#include "visitor.h"

namespace cythonpp::domain::ast {

// `continue`. Carries nothing but its span.
class Continue : public Stmt {
public:
    explicit Continue(SourceSpan span) : Stmt(span) {}

    void accept(Visitor& visitor) const override { visitor.visit(*this); }
};

} // namespace cythonpp::domain::ast

#endif // CYTHONPP_DOMAIN_AST_CONTINUE_H
