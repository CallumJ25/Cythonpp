#ifndef CYTHONPP_DOMAIN_AST_PASS_H
#define CYTHONPP_DOMAIN_AST_PASS_H

#include "stmt.h"
#include "visitor.h"

namespace cythonpp::domain::ast {

// `pass`. Carries nothing but its span.
class Pass : public Stmt {
public:
    explicit Pass(SourceSpan span) : Stmt(span) {}

    void accept(Visitor& visitor) const override { visitor.visit(*this); }
};

} // namespace cythonpp::domain::ast

#endif // CYTHONPP_DOMAIN_AST_PASS_H
