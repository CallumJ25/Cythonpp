#ifndef CYTHONPP_DOMAIN_AST_FUNCTION_DEF_H
#define CYTHONPP_DOMAIN_AST_FUNCTION_DEF_H

#include <string>
#include <utility>
#include <vector>

#include "expr.h"
#include "parameter.h"
#include "stmt.h"
#include "visitor.h"

namespace cythonpp::domain::ast {

// `def name(params) -> annotation: body`.
//
// Decorators and async are out of the supported subset. The return annotation
// may be null: an unannotated def is still syntactically valid, and rejecting
// it is the type checker's call, not the parser's.
class FunctionDef : public Stmt {
public:
    FunctionDef(SourceSpan span, std::string name, std::vector<Parameter> params,
                ExprPtr return_annotation, std::vector<StmtPtr> body)
        : Stmt(span),
          name_(std::move(name)),
          params_(std::move(params)),
          return_annotation_(std::move(return_annotation)),
          body_(std::move(body)) {}

    const std::string& name() const { return name_; }
    const std::vector<Parameter>& params() const { return params_; }
    bool has_return_annotation() const { return return_annotation_ != nullptr; }
    const Expr& return_annotation() const { return *return_annotation_; }
    const std::vector<StmtPtr>& body() const { return body_; }

    void accept(Visitor& visitor) const override { visitor.visit(*this); }

private:
    std::string name_;
    std::vector<Parameter> params_;
    ExprPtr return_annotation_;
    std::vector<StmtPtr> body_;
};

} // namespace cythonpp::domain::ast

#endif // CYTHONPP_DOMAIN_AST_FUNCTION_DEF_H
