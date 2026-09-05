#ifndef CYTHONPP_DOMAIN_AST_AST_PRINTER_H
#define CYTHONPP_DOMAIN_AST_AST_PRINTER_H

#include <string>
#include <vector>

#include "node.h"
#include "stmt.h"
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

    void visit(const AnnAssign& node) override;
    void visit(const Assign& node) override;
    void visit(const Attribute& node) override;
    void visit(const BinOp& node) override;
    void visit(const BoolOp& node) override;
    void visit(const Break& node) override;
    void visit(const Call& node) override;
    void visit(const ClassDef& node) override;
    void visit(const Compare& node) override;
    void visit(const Constant& node) override;
    void visit(const Continue& node) override;
    void visit(const DictExpr& node) override;
    void visit(const For& node) override;
    void visit(const FunctionDef& node) override;
    void visit(const If& node) override;
    void visit(const ListComp& node) override;
    void visit(const ListExpr& node) override;
    void visit(const Module& node) override;
    void visit(const Name& node) override;
    void visit(const Pass& node) override;
    void visit(const Return& node) override;
    void visit(const Subscript& node) override;
    void visit(const TupleExpr& node) override;
    void visit(const UnaryOp& node) override;
    void visit(const While& node) override;

private:
    std::string out_;
    int depth_ = 0;

    void newline_indent();

    // Renders each statement on its own line, one level deeper. Shared by
    // every node with a suite, so the indentation rule lives in one place.
    void print_body(const std::vector<StmtPtr>& body);

    // The optional `else` suite that If, While and For all carry. Emits
    // nothing when `orelse` is empty, so each caller is one unconditional
    // line rather than three copies of the same guard.
    void print_else(const std::vector<StmtPtr>& orelse);
};

} // namespace cythonpp::domain::ast

#endif // CYTHONPP_DOMAIN_AST_AST_PRINTER_H
