#ifndef CYTHONPP_DOMAIN_AST_UNARY_OP_H
#define CYTHONPP_DOMAIN_AST_UNARY_OP_H

#include <utility>

#include "domain/lexer/token_type.h"
#include "expr.h"
#include "visitor.h"

namespace cythonpp::domain::ast {

// A prefix operation: `-x`, `~mask`, `not flag`.
class UnaryOp : public Expr {
public:
    UnaryOp(SourceSpan span, lexer::token_type op, ExprPtr operand)
        : Expr(span), op_(op), operand_(std::move(operand)) {}

    lexer::token_type op() const { return op_; }
    const Expr& operand() const { return *operand_; }

    void accept(Visitor& visitor) const override { visitor.visit(*this); }

private:
    lexer::token_type op_;
    ExprPtr operand_;
};

} // namespace cythonpp::domain::ast

#endif // CYTHONPP_DOMAIN_AST_UNARY_OP_H
