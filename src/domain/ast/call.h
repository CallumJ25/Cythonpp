#ifndef CYTHONPP_DOMAIN_AST_CALL_H
#define CYTHONPP_DOMAIN_AST_CALL_H

#include <utility>
#include <vector>

#include "expr.h"
#include "visitor.h"

namespace cythonpp::domain::ast {

// A call: `f(x, 2)`, `obj.method()`. Only positional arguments -- keyword
// arguments and star-args are out of scope until the compiler handles them.
class Call : public Expr {
public:
    Call(SourceSpan span, ExprPtr callee, std::vector<ExprPtr> args)
        : Expr(span), callee_(std::move(callee)), args_(std::move(args)) {}

    const Expr& callee() const { return *callee_; }
    const std::vector<ExprPtr>& args() const { return args_; }

    void accept(Visitor& visitor) const override { visitor.visit(*this); }

private:
    ExprPtr callee_;
    std::vector<ExprPtr> args_;
};

} // namespace cythonpp::domain::ast

#endif // CYTHONPP_DOMAIN_AST_CALL_H
