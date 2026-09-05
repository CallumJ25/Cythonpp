#ifndef CYTHONPP_DOMAIN_AST_CLASS_DEF_H
#define CYTHONPP_DOMAIN_AST_CLASS_DEF_H

#include <string>
#include <utility>
#include <vector>

#include "expr.h"
#include "stmt.h"
#include "visitor.h"

namespace cythonpp::domain::ast {

// `class Name(bases): body`.
//
// Bases are Exprs because a base can be `Generic[T]`, not just a bare name.
// Keyword arguments in the base list -- metaclass= and friends -- are out of
// the supported subset.
class ClassDef : public Stmt {
public:
    ClassDef(SourceSpan span, std::string name, std::vector<ExprPtr> bases,
             std::vector<StmtPtr> body)
        : Stmt(span),
          name_(std::move(name)),
          bases_(std::move(bases)),
          body_(std::move(body)) {}

    const std::string& name() const { return name_; }
    const std::vector<ExprPtr>& bases() const { return bases_; }
    const std::vector<StmtPtr>& body() const { return body_; }

    void accept(Visitor& visitor) const override { visitor.visit(*this); }

private:
    std::string name_;
    std::vector<ExprPtr> bases_;
    std::vector<StmtPtr> body_;
};

} // namespace cythonpp::domain::ast

#endif // CYTHONPP_DOMAIN_AST_CLASS_DEF_H
