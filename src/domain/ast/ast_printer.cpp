#include "ast_printer.h"

#include "name.h"

namespace cythonpp::domain::ast {

std::string AstPrinter::print(const Node& node) {
    out_.clear();
    depth_ = 0;
    node.accept(*this);
    return out_;
}

void AstPrinter::visit(const Name& node) {
    out_ += "(Name " + node.identifier() + ")";
}

} // namespace cythonpp::domain::ast
