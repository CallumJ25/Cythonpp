#ifndef CYTHONPP_DOMAIN_AST_NAME_H
#define CYTHONPP_DOMAIN_AST_NAME_H

#include <string>
#include <utility>

#include "expr.h"
#include "visitor.h"

namespace cythonpp::domain::ast {

// A bare identifier in expression position: `total`, `self`, `MyClass`.
class Name : public Expr {
public:
    Name(SourceSpan span, std::string identifier)
        : Expr(span), identifier_(std::move(identifier)) {}

    const std::string& identifier() const { return identifier_; }

    void accept(Visitor& visitor) const override { visitor.visit(*this); }

private:
    std::string identifier_;
};

} // namespace cythonpp::domain::ast

#endif // CYTHONPP_DOMAIN_AST_NAME_H
