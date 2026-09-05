#ifndef CYTHONPP_DOMAIN_AST_SUBSCRIPT_H
#define CYTHONPP_DOMAIN_AST_SUBSCRIPT_H

#include <utility>

#include "expr.h"
#include "visitor.h"

namespace cythonpp::domain::ast {

// Indexing: `items[0]`, and also the generic annotations the lexer already
// treats specially, such as `list[int]`.
class Subscript : public Expr {
public:
    Subscript(SourceSpan span, ExprPtr value, ExprPtr index)
        : Expr(span), value_(std::move(value)), index_(std::move(index)) {}

    const Expr& value() const { return *value_; }
    const Expr& index() const { return *index_; }

    void accept(Visitor& visitor) const override { visitor.visit(*this); }

private:
    ExprPtr value_;
    ExprPtr index_;
};

} // namespace cythonpp::domain::ast

#endif // CYTHONPP_DOMAIN_AST_SUBSCRIPT_H
