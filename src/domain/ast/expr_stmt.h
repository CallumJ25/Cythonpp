#ifndef CYTHONPP_DOMAIN_AST_EXPR_STMT_H
#define CYTHONPP_DOMAIN_AST_EXPR_STMT_H

#include <utility>

#include "expr.h"
#include "stmt.h"
#include "visitor.h"

namespace cythonpp::domain::ast {

// An expression evaluated for its effect, with the value discarded:
// `print(x)`, `obj.method()`, or a docstring, which in Python is a bare
// string literal statement.
//
// Named ExprStmt rather than CPython's `Expr` because Expr is already the
// base class of the thirteen expression nodes.
//
// value_ is never null, deliberately unlike Return::value_ and
// AnnAssign::value_. An ExprStmt with no expression is not a thing: a parser
// that could not parse the expression does not build the node at all. So
// there is no has_value() and value() dereferences unconditionally.
class ExprStmt : public Stmt {
public:
    ExprStmt(SourceSpan span, ExprPtr value) : Stmt(span), value_(std::move(value)) {}

    const Expr& value() const { return *value_; }

    void accept(Visitor& visitor) const override { visitor.visit(*this); }

private:
    ExprPtr value_;
};

} // namespace cythonpp::domain::ast

#endif // CYTHONPP_DOMAIN_AST_EXPR_STMT_H
