#ifndef CYTHONPP_DOMAIN_AST_RETURN_H
#define CYTHONPP_DOMAIN_AST_RETURN_H

#include <utility>

#include "expr.h"
#include "stmt.h"
#include "visitor.h"

namespace cythonpp::domain::ast {

// `return` or `return expr`.
//
// A null value_ means a bare return, following the same convention as
// AnnAssign::value_, FunctionDef::return_annotation_, and Parameter's
// annotation/default_value: has_value() is the only supported way to ask,
// and value() dereferences unconditionally, so calling it on a bare return
// dereferences null.
class Return : public Stmt {
public:
    Return(SourceSpan span, ExprPtr value) : Stmt(span), value_(std::move(value)) {}

    bool has_value() const { return value_ != nullptr; }
    const Expr& value() const { return *value_; }

    void accept(Visitor& visitor) const override { visitor.visit(*this); }

private:
    ExprPtr value_;
};

} // namespace cythonpp::domain::ast

#endif // CYTHONPP_DOMAIN_AST_RETURN_H
