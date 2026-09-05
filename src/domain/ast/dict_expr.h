#ifndef CYTHONPP_DOMAIN_AST_DICT_EXPR_H
#define CYTHONPP_DOMAIN_AST_DICT_EXPR_H

#include <utility>
#include <vector>

#include "expr.h"
#include "visitor.h"

namespace cythonpp::domain::ast {

// A dict display: `{'k': 1}`. Entries are a vector of pairs rather than two
// parallel vectors, so a key and its value cannot get out of step.
class DictExpr : public Expr {
public:
    struct Entry {
        ExprPtr key;
        ExprPtr value;
    };

    DictExpr(SourceSpan span, std::vector<Entry> entries)
        : Expr(span), entries_(std::move(entries)) {}

    const std::vector<Entry>& entries() const { return entries_; }

    void accept(Visitor& visitor) const override { visitor.visit(*this); }

private:
    std::vector<Entry> entries_;
};

} // namespace cythonpp::domain::ast

#endif // CYTHONPP_DOMAIN_AST_DICT_EXPR_H
