#ifndef CYTHONPP_DOMAIN_AST_AST_PRINTER_H
#define CYTHONPP_DOMAIN_AST_AST_PRINTER_H

#include <string>

#include "node.h"
#include "visitor.h"

namespace cythonpp::domain::ast {

// Renders a tree as a canonical s-expression, for tests and for debugging the
// parser once one exists.
//
// Statements each begin a new line, indented two spaces per level; expressions
// render inline. Spans are deliberately not printed: including them would make
// every assertion noisy and couple printer tests to position arithmetic they
// are not testing.
class AstPrinter : public Visitor {
public:
    std::string print(const Node& node);

    void visit(const Attribute& node) override;
    void visit(const BinOp& node) override;
    void visit(const BoolOp& node) override;
    void visit(const Call& node) override;
    void visit(const Compare& node) override;
    void visit(const Constant& node) override;
    void visit(const DictExpr& node) override;
    void visit(const ListExpr& node) override;
    void visit(const Name& node) override;
    void visit(const Subscript& node) override;
    void visit(const TupleExpr& node) override;
    void visit(const UnaryOp& node) override;

private:
    std::string out_;
    int depth_ = 0;
};

} // namespace cythonpp::domain::ast

#endif // CYTHONPP_DOMAIN_AST_AST_PRINTER_H
