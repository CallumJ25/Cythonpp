#ifndef CYTHONPP_DOMAIN_AST_LIST_COMP_H
#define CYTHONPP_DOMAIN_AST_LIST_COMP_H

#include <utility>
#include <vector>

#include "comprehension_clause.h"
#include "expr.h"
#include "visitor.h"

namespace cythonpp::domain::ast {

// A list comprehension: `[x * 2 for x in items if x > 0]`.
//
// DictComp and SetComp are deliberately absent until something needs them.
// Merging all three into one node with a kind enum would discard exactly the
// static distinction this hierarchy exists to provide.
class ListComp : public Expr {
public:
    ListComp(SourceSpan span, ExprPtr element, std::vector<ComprehensionClause> clauses)
        : Expr(span), element_(std::move(element)), clauses_(std::move(clauses)) {}

    const Expr& element() const { return *element_; }
    const std::vector<ComprehensionClause>& clauses() const { return clauses_; }

    void accept(Visitor& visitor) const override { visitor.visit(*this); }

private:
    ExprPtr element_;
    std::vector<ComprehensionClause> clauses_;
};

} // namespace cythonpp::domain::ast

#endif // CYTHONPP_DOMAIN_AST_LIST_COMP_H
