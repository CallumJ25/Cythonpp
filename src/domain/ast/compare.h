#ifndef CYTHONPP_DOMAIN_AST_COMPARE_H
#define CYTHONPP_DOMAIN_AST_COMPARE_H

#include <utility>
#include <vector>

#include "domain/lexer/token_type.h"
#include "expr.h"
#include "visitor.h"

namespace cythonpp::domain::ast {

// A comparison chain: `a < b`, and also `a < b <= c`, which Python evaluates
// as `a < b and b <= c` with `b` computed once. Keeping the chain intact
// rather than desugaring here is what lets a later stage emit that single
// evaluation.
class Compare : public Expr {
public:
    struct Rest {
        lexer::token_type op;
        ExprPtr operand;
    };

    Compare(SourceSpan span, ExprPtr left, std::vector<Rest> rest)
        : Expr(span), left_(std::move(left)), rest_(std::move(rest)) {}

    const Expr& left() const { return *left_; }
    const std::vector<Rest>& rest() const { return rest_; }

    void accept(Visitor& visitor) const override { visitor.visit(*this); }

private:
    ExprPtr left_;
    std::vector<Rest> rest_;
};

} // namespace cythonpp::domain::ast

#endif // CYTHONPP_DOMAIN_AST_COMPARE_H
