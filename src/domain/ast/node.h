#ifndef CYTHONPP_DOMAIN_AST_NODE_H
#define CYTHONPP_DOMAIN_AST_NODE_H

#include "source_span.h"

namespace cythonpp::domain::ast {

class Visitor;

// Base of every AST node.
//
// Nodes are immutable once built and are not copyable: a tree owns its
// children through unique_ptr, so a copy would either be a shallow alias or a
// deep clone, and neither has an obvious right answer until something needs
// one. Copying is deleted rather than left to the compiler so the question
// surfaces as a compile error.
class Node {
public:
    explicit Node(SourceSpan span) : span_(span) {}
    virtual ~Node() = default;

    Node(const Node&) = delete;
    Node& operator=(const Node&) = delete;

    SourceSpan span() const { return span_; }

    virtual void accept(Visitor& visitor) const = 0;

private:
    SourceSpan span_;
};

} // namespace cythonpp::domain::ast

#endif // CYTHONPP_DOMAIN_AST_NODE_H
