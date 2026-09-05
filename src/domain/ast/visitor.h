#ifndef CYTHONPP_DOMAIN_AST_VISITOR_H
#define CYTHONPP_DOMAIN_AST_VISITOR_H

namespace cythonpp::domain::ast {

class Attribute;
class BinOp;
class BoolOp;
class Call;
class Compare;
class Constant;
class DictExpr;
class ListExpr;
class Name;
class Subscript;
class TupleExpr;
class UnaryOp;

// Traversal over the node hierarchy.
//
// Every method is pure virtual on purpose. The node set is still growing, and
// adding a node should break every visitor until it is handled -- a compile
// error at exactly the places that need attention. A defaulted no-op would
// turn "added a node, forgot to emit code for it" into missing output at
// runtime instead.
//
// WARNING for whoever adds a RecursiveVisitor with non-pure defaults: a
// derived class that declares one `visit` override hides *every* other
// `visit` overload from the base. That is harmless here, because all methods
// are pure and so every visitor overrides all of them. It stops being
// harmless the moment defaults exist -- subclasses will need
// `using RecursiveVisitor::visit;` or traversal will silently stop.
class Visitor {
public:
    virtual ~Visitor() = default;

    virtual void visit(const Attribute& node) = 0;
    virtual void visit(const BinOp& node) = 0;
    virtual void visit(const BoolOp& node) = 0;
    virtual void visit(const Call& node) = 0;
    virtual void visit(const Compare& node) = 0;
    virtual void visit(const Constant& node) = 0;
    virtual void visit(const DictExpr& node) = 0;
    virtual void visit(const ListExpr& node) = 0;
    virtual void visit(const Name& node) = 0;
    virtual void visit(const Subscript& node) = 0;
    virtual void visit(const TupleExpr& node) = 0;
    virtual void visit(const UnaryOp& node) = 0;
};

} // namespace cythonpp::domain::ast

#endif // CYTHONPP_DOMAIN_AST_VISITOR_H
