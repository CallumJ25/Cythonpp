#ifndef CYTHONPP_DOMAIN_AST_MODULE_H
#define CYTHONPP_DOMAIN_AST_MODULE_H

#include <utility>
#include <vector>

#include "node.h"
#include "stmt.h"
#include "visitor.h"

namespace cythonpp::domain::ast {

// One source file's top-level statements.
//
// Derives from Node rather than Stmt: a module is the root of a tree, never a
// child of anything, and making it a Stmt would let it be nested inside a
// function body.
class Module : public Node {
public:
    Module(SourceSpan span, std::vector<StmtPtr> body) : Node(span), body_(std::move(body)) {}

    const std::vector<StmtPtr>& body() const { return body_; }

    void accept(Visitor& visitor) const override { visitor.visit(*this); }

private:
    std::vector<StmtPtr> body_;
};

} // namespace cythonpp::domain::ast

#endif // CYTHONPP_DOMAIN_AST_MODULE_H
