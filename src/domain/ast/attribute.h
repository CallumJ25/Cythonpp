#ifndef CYTHONPP_DOMAIN_AST_ATTRIBUTE_H
#define CYTHONPP_DOMAIN_AST_ATTRIBUTE_H

#include <string>
#include <utility>

#include "expr.h"
#include "visitor.h"

namespace cythonpp::domain::ast {

// Attribute access: `self.count`, `module.function`.
class Attribute : public Expr {
public:
    Attribute(SourceSpan span, ExprPtr value, std::string attribute)
        : Expr(span), value_(std::move(value)), attribute_(std::move(attribute)) {}

    const Expr& value() const { return *value_; }
    const std::string& attribute() const { return attribute_; }

    void accept(Visitor& visitor) const override { visitor.visit(*this); }

private:
    ExprPtr value_;
    std::string attribute_;
};

} // namespace cythonpp::domain::ast

#endif // CYTHONPP_DOMAIN_AST_ATTRIBUTE_H
