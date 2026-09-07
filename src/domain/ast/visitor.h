#ifndef CYTHONPP_DOMAIN_AST_VISITOR_H
#define CYTHONPP_DOMAIN_AST_VISITOR_H

namespace cythonpp::domain::ast {

class AnnAssign;
class Assign;
class Attribute;
class BinOp;
class BoolOp;
class Break;
class Call;
class ClassDef;
class Compare;
class Constant;
class Continue;
class DictExpr;
class ExprStmt;
class For;
class FunctionDef;
class If;
class ListComp;
class ListExpr;
class Module;
class Name;
class Pass;
class Return;
class Subscript;
class TupleExpr;
class UnaryOp;
class While;

// Traversal over the node hierarchy.
//
// Every method is pure virtual on purpose. The node set is still growing, and
// adding a node should break every visitor until it is handled -- a compile
// error at exactly the places that need attention. A defaulted no-op would
// turn "added a node, forgot to emit code for it" into missing output at
// runtime instead.
//
// NOTE for whoever adds a RecursiveVisitor with non-pure defaults: a derived
// class that declares one `visit` override does hide *every* other `visit`
// overload from the base, by ordinary name-hiding rules. That is harmless
// here, because all methods are pure and so every visitor overrides all of
// them.
//
// It is also less dangerous than it looks once defaults exist, and the
// earlier version of this comment had it wrong. Traversal does NOT silently
// stop: accept() dispatches through Visitor&, and virtual dispatch is
// unaffected by name hiding, so a subclass overriding one method still has
// the base's defaults fire for the other twenty-five and still receives its
// own override when one is reached. Verified by experiment.
//
// What hiding actually costs is narrower and louder: an UNQUALIFIED
// `visit(child)` written inside such a subclass fails to compile, because
// only the overloads that subclass declared are visible to name lookup --
// `error: no viable conversion from 'const A' to 'const B'`. The fix is
// `using RecursiveVisitor::visit;` in the subclass, or a qualified
// `RecursiveVisitor::visit(node)` call. Either way it is a build failure, not
// a runtime one.
class Visitor {
public:
    virtual ~Visitor() = default;

    virtual void visit(const AnnAssign& node) = 0;
    virtual void visit(const Assign& node) = 0;
    virtual void visit(const Attribute& node) = 0;
    virtual void visit(const BinOp& node) = 0;
    virtual void visit(const BoolOp& node) = 0;
    virtual void visit(const Break& node) = 0;
    virtual void visit(const Call& node) = 0;
    virtual void visit(const ClassDef& node) = 0;
    virtual void visit(const Compare& node) = 0;
    virtual void visit(const Constant& node) = 0;
    virtual void visit(const Continue& node) = 0;
    virtual void visit(const DictExpr& node) = 0;
    virtual void visit(const ExprStmt& node) = 0;
    virtual void visit(const For& node) = 0;
    virtual void visit(const FunctionDef& node) = 0;
    virtual void visit(const If& node) = 0;
    virtual void visit(const ListComp& node) = 0;
    virtual void visit(const ListExpr& node) = 0;
    virtual void visit(const Module& node) = 0;
    virtual void visit(const Name& node) = 0;
    virtual void visit(const Pass& node) = 0;
    virtual void visit(const Return& node) = 0;
    virtual void visit(const Subscript& node) = 0;
    virtual void visit(const TupleExpr& node) = 0;
    virtual void visit(const UnaryOp& node) = 0;
    virtual void visit(const While& node) = 0;
};

} // namespace cythonpp::domain::ast

#endif // CYTHONPP_DOMAIN_AST_VISITOR_H
