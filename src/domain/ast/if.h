#ifndef CYTHONPP_DOMAIN_AST_IF_H
#define CYTHONPP_DOMAIN_AST_IF_H

#include <utility>
#include <vector>

#include "expr.h"
#include "stmt.h"
#include "visitor.h"

namespace cythonpp::domain::ast {

// `if cond: body` with an optional `else: orelse`.
//
// `elif` is not a separate node: the parser nests it as an If inside the
// orelse of the outer one, which is how Python's own grammar defines it.
class If : public Stmt {
public:
    If(SourceSpan span, ExprPtr condition, std::vector<StmtPtr> body,
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

#endif // CYTHONPP_DOMAIN_AST_IF_H
