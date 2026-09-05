#include "ast_printer.h"

#include <cstddef>

#include "ann_assign.h"
#include "assign.h"
#include "attribute.h"
#include "bin_op.h"
#include "bool_op.h"
#include "break.h"
#include "call.h"
#include "compare.h"
#include "constant.h"
#include "continue.h"
#include "dict_expr.h"
#include "domain/lexer/keyword_table.h"
#include "domain/lexer/operator_table.h"
#include "domain/lexer/token_type_name.h"
#include "for.h"
#include "if.h"
#include "list_comp.h"
#include "list_expr.h"
#include "name.h"
#include "pass.h"
#include "return.h"
#include "subscript.h"
#include "tuple_expr.h"
#include "unary_op.h"
#include "while.h"

namespace cythonpp::domain::ast {

namespace {

// How an operator is spelled in source. Punctuation first, then word-shaped
// operators like `and` and `is`, then the enumerator name as a last resort so
// a token_type with no spelling prints something identifiable rather than
// nothing at all.
std::string op_spelling(lexer::token_type type) {
    std::string_view spelling = lexer::operator_lexeme_of(type);
    if (!spelling.empty()) {
        return std::string(spelling);
    }
    spelling = lexer::keyword_lexeme_of(type);
    if (!spelling.empty()) {
        return std::string(spelling);
    }
    return std::string(lexer::token_type_name(type));
}

} // namespace

std::string AstPrinter::print(const Node& node) {
    out_.clear();
    depth_ = 0;
    node.accept(*this);
    return out_;
}

void AstPrinter::visit(const AnnAssign& node) {
    out_ += "(AnnAssign ";
    node.target().accept(*this);
    out_ += " ";
    node.annotation().accept(*this);
    if (node.has_value()) {
        out_ += " ";
        node.value().accept(*this);
    }
    out_ += ")";
}

void AstPrinter::visit(const Assign& node) {
    out_ += "(Assign ";
    node.target().accept(*this);
    out_ += " ";
    node.value().accept(*this);
    out_ += ")";
}

void AstPrinter::visit(const Attribute& node) {
    out_ += "(Attribute ";
    node.value().accept(*this);
    out_ += " " + node.attribute() + ")";
}

void AstPrinter::visit(const BinOp& node) {
    out_ += "(BinOp " + op_spelling(node.op()) + " ";
    node.left().accept(*this);
    out_ += " ";
    node.right().accept(*this);
    out_ += ")";
}

void AstPrinter::visit(const BoolOp& node) {
    out_ += "(BoolOp " + op_spelling(node.op());
    for (const ExprPtr& value : node.values()) {
        out_ += " ";
        value->accept(*this);
    }
    out_ += ")";
}

void AstPrinter::visit(const Break&) { out_ += "(Break)"; }

void AstPrinter::visit(const Call& node) {
    out_ += "(Call ";
    node.callee().accept(*this);
    for (const ExprPtr& arg : node.args()) {
        out_ += " ";
        arg->accept(*this);
    }
    out_ += ")";
}

void AstPrinter::visit(const Compare& node) {
    out_ += "(Compare ";
    node.left().accept(*this);
    for (const Compare::Rest& rest : node.rest()) {
        out_ += " " + op_spelling(rest.op) + " ";
        rest.operand->accept(*this);
    }
    out_ += ")";
}

void AstPrinter::visit(const Constant& node) {
    out_ += "(Constant " + node.lexeme() + ")";
}

void AstPrinter::visit(const Continue&) { out_ += "(Continue)"; }

void AstPrinter::visit(const DictExpr& node) {
    out_ += "(DictExpr";
    for (const DictExpr::Entry& entry : node.entries()) {
        out_ += " (";
        entry.key->accept(*this);
        out_ += " ";
        entry.value->accept(*this);
        out_ += ")";
    }
    out_ += ")";
}

void AstPrinter::newline_indent() {
    out_ += "\n";
    out_.append(static_cast<std::size_t>(2 * depth_), ' ');
}

void AstPrinter::print_body(const std::vector<StmtPtr>& body) {
    ++depth_;
    for (const StmtPtr& statement : body) {
        newline_indent();
        statement->accept(*this);
    }
    --depth_;
}

void AstPrinter::print_else(const std::vector<StmtPtr>& orelse) {
    if (orelse.empty()) {
        return;
    }
    ++depth_;
    newline_indent();
    out_ += "(Else";
    print_body(orelse);
    out_ += ")";
    --depth_;
}

void AstPrinter::visit(const For& node) {
    out_ += "(For ";
    node.target().accept(*this);
    out_ += " ";
    node.iterable().accept(*this);
    print_body(node.body());
    print_else(node.orelse());
    out_ += ")";
}

void AstPrinter::visit(const If& node) {
    out_ += "(If ";
    node.condition().accept(*this);
    print_body(node.body());
    print_else(node.orelse());
    out_ += ")";
}

void AstPrinter::visit(const ListComp& node) {
    out_ += "(ListComp ";
    node.element().accept(*this);
    for (const ComprehensionClause& clause : node.clauses()) {
        out_ += " (Clause ";
        clause.target->accept(*this);
        out_ += " ";
        clause.iterable->accept(*this);
        for (const ExprPtr& condition : clause.conditions) {
            out_ += " ";
            condition->accept(*this);
        }
        out_ += ")";
    }
    out_ += ")";
}

void AstPrinter::visit(const ListExpr& node) {
    out_ += "(ListExpr";
    for (const ExprPtr& element : node.elements()) {
        out_ += " ";
        element->accept(*this);
    }
    out_ += ")";
}

void AstPrinter::visit(const Name& node) {
    out_ += "(Name " + node.identifier() + ")";
}

void AstPrinter::visit(const Pass&) { out_ += "(Pass)"; }

void AstPrinter::visit(const Return& node) {
    out_ += "(Return";
    if (node.has_value()) {
        out_ += " ";
        node.value().accept(*this);
    }
    out_ += ")";
}

void AstPrinter::visit(const Subscript& node) {
    out_ += "(Subscript ";
    node.value().accept(*this);
    out_ += " ";
    node.index().accept(*this);
    out_ += ")";
}

void AstPrinter::visit(const TupleExpr& node) {
    out_ += "(TupleExpr";
    for (const ExprPtr& element : node.elements()) {
        out_ += " ";
        element->accept(*this);
    }
    out_ += ")";
}

void AstPrinter::visit(const UnaryOp& node) {
    out_ += "(UnaryOp " + op_spelling(node.op()) + " ";
    node.operand().accept(*this);
    out_ += ")";
}

void AstPrinter::visit(const While& node) {
    out_ += "(While ";
    node.condition().accept(*this);
    print_body(node.body());
    print_else(node.orelse());
    out_ += ")";
}

} // namespace cythonpp::domain::ast
