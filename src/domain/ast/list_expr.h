#ifndef CYTHONPP_DOMAIN_AST_LIST_EXPR_H
#define CYTHONPP_DOMAIN_AST_LIST_EXPR_H

#include <utility>
#include <vector>

#include "expr.h"
#include "visitor.h"

namespace cythonpp::domain::ast {

// A list display: `[1, 2, 3]`. Named ListExpr rather than CPython's List so
// it does not read like std::list at a glance.
class ListExpr : public Expr {
public:
    ListExpr(SourceSpan span, std::vector<ExprPtr> elements)
        : Expr(span), elements_(std::move(elements)) {}

    const std::vector<ExprPtr>& elements() const { return elements_; }

    void accept(Visitor& visitor) const override { visitor.visit(*this); }

private:
    std::vector<ExprPtr> elements_;
};

} // namespace cythonpp::domain::ast

#endif // CYTHONPP_DOMAIN_AST_LIST_EXPR_H
