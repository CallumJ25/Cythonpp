#ifndef CYTHONPP_DOMAIN_AST_EXPR_H
#define CYTHONPP_DOMAIN_AST_EXPR_H

#include <memory>

#include "node.h"

namespace cythonpp::domain::ast {

// An expression: something that evaluates to a value. Exists so the type
// system encodes the grammar -- If::condition() returns const Expr&, so
// wiring a statement into a condition does not compile.
class Expr : public Node {
public:
    using Node::Node;
};

using ExprPtr = std::unique_ptr<Expr>;

} // namespace cythonpp::domain::ast

#endif // CYTHONPP_DOMAIN_AST_EXPR_H
