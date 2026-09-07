#include "domain/ast/recursive_visitor.h"

#include <vector>

#include "domain/ast/ann_assign.h"
#include "domain/ast/assign.h"
#include "domain/ast/attribute.h"
#include "domain/ast/bin_op.h"
#include "domain/ast/bool_op.h"
#include "domain/ast/break.h"
#include "domain/ast/call.h"
#include "domain/ast/class_def.h"
#include "domain/ast/compare.h"
#include "domain/ast/comprehension_clause.h"
#include "domain/ast/constant.h"
#include "domain/ast/continue.h"
#include "domain/ast/dict_expr.h"
#include "domain/ast/expr_stmt.h"
#include "domain/ast/for.h"
#include "domain/ast/function_def.h"
#include "domain/ast/if.h"
#include "domain/ast/list_comp.h"
#include "domain/ast/list_expr.h"
#include "domain/ast/module.h"
#include "domain/ast/name.h"
#include "domain/ast/parameter.h"
#include "domain/ast/pass.h"
#include "domain/ast/return.h"
#include "domain/ast/subscript.h"
#include "domain/ast/tuple_expr.h"
#include "domain/ast/unary_op.h"
#include "domain/ast/while.h"

namespace cythonpp::domain::ast {
namespace {

// Children go through accept(), never through a direct visit() call. accept()
// goes via the vtable, so a subclass's override is reached; a direct
// this->visit(child) would bind at compile time to whatever overload name
// lookup found in THIS class -- the base's -- silently skipping the subclass.
void walk_body(const std::vector<StmtPtr>& body, Visitor& visitor) {
    for (const StmtPtr& statement : body) {
        statement->accept(visitor);
    }
}

void walk_exprs(const std::vector<ExprPtr>& exprs, Visitor& visitor) {
    for (const ExprPtr& expr : exprs) {
        expr->accept(visitor);
    }
}

} // namespace

void RecursiveVisitor::visit(const AnnAssign& node) {
    node.target().accept(*this);
    node.annotation().accept(*this);
    // Guarded: value() dereferences unconditionally.
    if (node.has_value()) {
        node.value().accept(*this);
    }
}

void RecursiveVisitor::visit(const Assign& node) {
    node.target().accept(*this);
    node.value().accept(*this);
}

void RecursiveVisitor::visit(const Attribute& node) { node.value().accept(*this); }

void RecursiveVisitor::visit(const BinOp& node) {
    node.left().accept(*this);
    node.right().accept(*this);
}

void RecursiveVisitor::visit(const BoolOp& node) { walk_exprs(node.values(), *this); }

void RecursiveVisitor::visit(const Break&) {}

void RecursiveVisitor::visit(const Call& node) {
    node.callee().accept(*this);
    walk_exprs(node.args(), *this);
}

void RecursiveVisitor::visit(const ClassDef& node) {
    walk_exprs(node.bases(), *this);
    walk_body(node.body(), *this);
}

void RecursiveVisitor::visit(const Compare& node) {
    node.left().accept(*this);
    for (const Compare::Rest& rest : node.rest()) {
        // A public ExprPtr field on the nested struct, not an accessor.
        rest.operand->accept(*this);
    }
}

void RecursiveVisitor::visit(const Constant&) {}
void RecursiveVisitor::visit(const Continue&) {}

void RecursiveVisitor::visit(const DictExpr& node) {
    for (const DictExpr::Entry& entry : node.entries()) {
        entry.key->accept(*this);
        entry.value->accept(*this);
    }
}

void RecursiveVisitor::visit(const ExprStmt& node) {
    // Unguarded, and never null: a parser that could not parse the expression
    // never builds the node at all.
    node.value().accept(*this);
}

void RecursiveVisitor::visit(const For& node) {
    node.target().accept(*this);
    node.iterable().accept(*this);
    walk_body(node.body(), *this);
    walk_body(node.orelse(), *this);
}

void RecursiveVisitor::visit(const FunctionDef& node) {
    for (const Parameter& parameter : node.params()) {
        // Raw ExprPtr fields with NO has_*() guard -- check against nullptr.
        if (parameter.annotation) {
            parameter.annotation->accept(*this);
        }
        if (parameter.default_value) {
            parameter.default_value->accept(*this);
        }
    }
    if (node.has_return_annotation()) {
        node.return_annotation().accept(*this);
    }
    walk_body(node.body(), *this);
}

void RecursiveVisitor::visit(const If& node) {
    node.condition().accept(*this);
    walk_body(node.body(), *this);
    walk_body(node.orelse(), *this);
}

void RecursiveVisitor::visit(const ListComp& node) {
    node.element().accept(*this);
    for (const ComprehensionClause& clause : node.clauses()) {
        clause.target->accept(*this);
        clause.iterable->accept(*this);
        // Easy to miss, and a miss silently leaves the filters unchecked.
        walk_exprs(clause.conditions, *this);
    }
}

void RecursiveVisitor::visit(const ListExpr& node) { walk_exprs(node.elements(), *this); }
void RecursiveVisitor::visit(const Module& node) { walk_body(node.body(), *this); }
void RecursiveVisitor::visit(const Name&) {}
void RecursiveVisitor::visit(const Pass&) {}

void RecursiveVisitor::visit(const Return& node) {
    // Guarded: value() dereferences unconditionally, and a bare `return` has
    // none.
    if (node.has_value()) {
        node.value().accept(*this);
    }
}

void RecursiveVisitor::visit(const Subscript& node) {
    node.value().accept(*this);
    node.index().accept(*this);
}

void RecursiveVisitor::visit(const TupleExpr& node) { walk_exprs(node.elements(), *this); }
void RecursiveVisitor::visit(const UnaryOp& node) { node.operand().accept(*this); }

void RecursiveVisitor::visit(const While& node) {
    node.condition().accept(*this);
    walk_body(node.body(), *this);
    walk_body(node.orelse(), *this);
}

} // namespace cythonpp::domain::ast
