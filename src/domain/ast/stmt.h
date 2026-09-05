#ifndef CYTHONPP_DOMAIN_AST_STMT_H
#define CYTHONPP_DOMAIN_AST_STMT_H

#include <memory>

#include "node.h"

namespace cythonpp::domain::ast {

// A statement: something executed for effect rather than evaluated.
class Stmt : public Node {
public:
    using Node::Node;
};

using StmtPtr = std::unique_ptr<Stmt>;

} // namespace cythonpp::domain::ast

#endif // CYTHONPP_DOMAIN_AST_STMT_H
