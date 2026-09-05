#ifndef CYTHONPP_DOMAIN_AST_ASSIGN_H
#define CYTHONPP_DOMAIN_AST_ASSIGN_H

#include <utility>

#include "expr.h"
#include "stmt.h"
#include "visitor.h"

namespace cythonpp::domain::ast {

// `target = value`. One target only: chained assignment (`a = b = 1`) is not
// in the supported subset, and unpacking is expressed by a TupleExpr target.
class Assign : public Stmt {
public:
    Assign(SourceSpan span, ExprPtr target, ExprPtr value)
        : Stmt(span), target_(std::move(target)), value_(std::move(value)) {}

    const Expr& target() const { return *target_; }
    const Expr& value() const { return *value_; }

    void accept(Visitor& visitor) const override { visitor.visit(*this); }

private:
    ExprPtr target_;
    ExprPtr value_;
};

} // namespace cythonpp::domain::ast

#endif // CYTHONPP_DOMAIN_AST_ASSIGN_H
