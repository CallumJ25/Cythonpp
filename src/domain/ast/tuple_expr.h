#ifndef CYTHONPP_DOMAIN_AST_TUPLE_EXPR_H
#define CYTHONPP_DOMAIN_AST_TUPLE_EXPR_H

#include <utility>
#include <vector>

#include "expr.h"
#include "visitor.h"

namespace cythonpp::domain::ast {

// A tuple display: `(a, b)`, and the implicit tuple in `x, y = 1, 2`.
class TupleExpr : public Expr {
public:
    TupleExpr(SourceSpan span, std::vector<ExprPtr> elements)
        : Expr(span), elements_(std::move(elements)) {}

    const std::vector<ExprPtr>& elements() const { return elements_; }

    void accept(Visitor& visitor) const override { visitor.visit(*this); }

private:
    std::vector<ExprPtr> elements_;
};

} // namespace cythonpp::domain::ast

#endif // CYTHONPP_DOMAIN_AST_TUPLE_EXPR_H
