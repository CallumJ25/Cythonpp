#ifndef CYTHONPP_DOMAIN_AST_BIN_OP_H
#define CYTHONPP_DOMAIN_AST_BIN_OP_H

#include <utility>

#include "domain/lexer/token_type.h"
#include "expr.h"
#include "visitor.h"

namespace cythonpp::domain::ast {

// A binary arithmetic or bitwise operation: `x + 1`, `a // b`.
//
// The operator is the lexer's token_type rather than a parallel ast enum, so
// there is no second table to keep in step with operator_table.cpp.
class BinOp : public Expr {
public:
    BinOp(SourceSpan span, lexer::token_type op, ExprPtr left, ExprPtr right)
        : Expr(span), op_(op), left_(std::move(left)), right_(std::move(right)) {}

    lexer::token_type op() const { return op_; }
    const Expr& left() const { return *left_; }
    const Expr& right() const { return *right_; }

    void accept(Visitor& visitor) const override { visitor.visit(*this); }

private:
    lexer::token_type op_;
    ExprPtr left_;
    ExprPtr right_;
};

} // namespace cythonpp::domain::ast

#endif // CYTHONPP_DOMAIN_AST_BIN_OP_H
