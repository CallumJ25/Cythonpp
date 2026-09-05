#ifndef CYTHONPP_DOMAIN_AST_FOR_H
#define CYTHONPP_DOMAIN_AST_FOR_H

#include <utility>
#include <vector>

#include "expr.h"
#include "stmt.h"
#include "visitor.h"

namespace cythonpp::domain::ast {

// `for target in iterable: body`, with an optional `else: orelse`.
//
// The target is an Expr, not a string, because `for i, item in pairs:` binds
// a TupleExpr.
class For : public Stmt {
public:
    For(SourceSpan span, ExprPtr target, ExprPtr iterable, std::vector<StmtPtr> body,
        std::vector<StmtPtr> orelse)
        : Stmt(span),
          target_(std::move(target)),
          iterable_(std::move(iterable)),
          body_(std::move(body)),
          orelse_(std::move(orelse)) {}

    const Expr& target() const { return *target_; }
    const Expr& iterable() const { return *iterable_; }
    const std::vector<StmtPtr>& body() const { return body_; }
    const std::vector<StmtPtr>& orelse() const { return orelse_; }

    void accept(Visitor& visitor) const override { visitor.visit(*this); }

private:
    ExprPtr target_;
    ExprPtr iterable_;
    std::vector<StmtPtr> body_;
    std::vector<StmtPtr> orelse_;
};

} // namespace cythonpp::domain::ast

#endif // CYTHONPP_DOMAIN_AST_FOR_H
