#ifndef CYTHONPP_DOMAIN_AST_WHILE_H
#define CYTHONPP_DOMAIN_AST_WHILE_H

#include <utility>
#include <vector>

#include "expr.h"
#include "stmt.h"
#include "visitor.h"

namespace cythonpp::domain::ast {

// `while cond: body` with an optional `else: orelse`, which Python runs when
// the loop finishes without hitting a break.
class While : public Stmt {
public:
    While(SourceSpan span, ExprPtr condition, std::vector<StmtPtr> body,
          std::vector<StmtPtr> orelse)
        : Stmt(span),
          condition_(std::move(condition)),
          body_(std::move(body)),
          orelse_(std::move(orelse)) {}

    const Expr& condition() const { return *condition_; }
    const std::vector<StmtPtr>& body() const { return body_; }
    const std::vector<StmtPtr>& orelse() const { return orelse_; }

    void accept(Visitor& visitor) const override { visitor.visit(*this); }

private:
    ExprPtr condition_;
    std::vector<StmtPtr> body_;
    std::vector<StmtPtr> orelse_;
};

} // namespace cythonpp::domain::ast

#endif // CYTHONPP_DOMAIN_AST_WHILE_H
