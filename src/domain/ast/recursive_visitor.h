#ifndef CYTHONPP_DOMAIN_AST_RECURSIVE_VISITOR_H
#define CYTHONPP_DOMAIN_AST_RECURSIVE_VISITOR_H

#include "visitor.h"

namespace cythonpp::domain::ast {

// A Visitor whose defaults walk the tree, so a subclass overrides only the
// nodes it cares about.
//
// The opposite default from AnnotationResolver's dynamic_cast dispatch, and
// deliberately: a TypeChecker that does not handle a node should still have
// its children checked, so the worst case is a missed rule. An annotation
// resolver wants the reverse -- an unrecognised annotation shape is an error,
// and recursing into it would turn "not a valid annotation" into a
// confidently wrong type. One base class cannot supply both defaults, which
// is why this arrived with its consumer rather than with the type algebra.
//
// NAME HIDING, and what it does and does not cost. A subclass declaring one
// `visit` override hides the other twenty-five from name lookup in that
// subclass's scope. Traversal is UNAFFECTED: accept() dispatches through
// Visitor&, virtual dispatch ignores name hiding, and the inherited defaults
// still fire. The only real symptom is that an UNQUALIFIED `visit(child)`
// written inside such a subclass does not compile ("no viable conversion from
// 'const A' to 'const B'"). The remedy is `using RecursiveVisitor::visit;` in
// the subclass, or a qualified `RecursiveVisitor::visit(node)` call. Either
// way it is a build failure, not a runtime one -- see
// recursive_visitor_test.cpp, which pins both halves.
class RecursiveVisitor : public Visitor {
public:
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
    void visit(const ExprStmt& node) override;
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
};

} // namespace cythonpp::domain::ast

#endif // CYTHONPP_DOMAIN_AST_RECURSIVE_VISITOR_H
