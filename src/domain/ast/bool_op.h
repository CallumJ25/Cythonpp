#ifndef CYTHONPP_DOMAIN_AST_BOOL_OP_H
#define CYTHONPP_DOMAIN_AST_BOOL_OP_H

#include <utility>
#include <vector>

#include "domain/lexer/token_type.h"
#include "expr.h"
#include "visitor.h"

namespace cythonpp::domain::ast {

// A chain of `and` or `or`. One node holds the whole run rather than nesting
// pairs, because `a and b and c` short-circuits as a single left-to-right
// sequence and codegen wants to see it that way.
class BoolOp : public Expr {
public:
    BoolOp(SourceSpan span, lexer::token_type op, std::vector<ExprPtr> values)
        : Expr(span), op_(op), values_(std::move(values)) {}

    lexer::token_type op() const { return op_; }
    const std::vector<ExprPtr>& values() const { return values_; }

    void accept(Visitor& visitor) const override { visitor.visit(*this); }

private:
    lexer::token_type op_;
    std::vector<ExprPtr> values_;
};

} // namespace cythonpp::domain::ast

#endif // CYTHONPP_DOMAIN_AST_BOOL_OP_H
