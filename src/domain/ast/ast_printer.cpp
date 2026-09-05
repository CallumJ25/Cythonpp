#include "ast_printer.h"

#include "attribute.h"
#include "bin_op.h"
#include "bool_op.h"
#include "compare.h"
#include "constant.h"
#include "domain/lexer/keyword_table.h"
#include "domain/lexer/operator_table.h"
#include "domain/lexer/token_type_name.h"
#include "name.h"
#include "subscript.h"
#include "unary_op.h"

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

void AstPrinter::visit(const Name& node) {
    out_ += "(Name " + node.identifier() + ")";
}

void AstPrinter::visit(const Subscript& node) {
    out_ += "(Subscript ";
    node.value().accept(*this);
    out_ += " ";
    node.index().accept(*this);
    out_ += ")";
}

void AstPrinter::visit(const UnaryOp& node) {
    out_ += "(UnaryOp " + op_spelling(node.op()) + " ";
    node.operand().accept(*this);
    out_ += ")";
}

} // namespace cythonpp::domain::ast
