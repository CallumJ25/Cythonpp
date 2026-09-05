#ifndef CYTHONPP_DOMAIN_AST_ANN_ASSIGN_H
#define CYTHONPP_DOMAIN_AST_ANN_ASSIGN_H

#include <utility>

#include "expr.h"
#include "stmt.h"
#include "visitor.h"

namespace cythonpp::domain::ast {

// An annotated assignment: `x: int = 5`, or a bare declaration `x: int`.
//
// The annotation is an ordinary Expr subtree, because Python annotations are
// syntactically expressions -- `int`, `list[int]`, `Optional[str]`. Turning
// one into a type is semantic analysis's job, not the parser's.
class AnnAssign : public Stmt {
public:
    AnnAssign(SourceSpan span, ExprPtr target, ExprPtr annotation, ExprPtr value)
        : Stmt(span),
          target_(std::move(target)),
          annotation_(std::move(annotation)),
          value_(std::move(value)) {}

    const Expr& target() const { return *target_; }
    const Expr& annotation() const { return *annotation_; }
    bool has_value() const { return value_ != nullptr; }
    const Expr& value() const { return *value_; }

    void accept(Visitor& visitor) const override { visitor.visit(*this); }

private:
    ExprPtr target_;
    ExprPtr annotation_;
    ExprPtr value_;
};

} // namespace cythonpp::domain::ast

#endif // CYTHONPP_DOMAIN_AST_ANN_ASSIGN_H
