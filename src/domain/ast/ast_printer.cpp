#include "ast_printer.h"

#include "attribute.h"
#include "constant.h"
#include "name.h"
#include "subscript.h"

namespace cythonpp::domain::ast {

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

} // namespace cythonpp::domain::ast
