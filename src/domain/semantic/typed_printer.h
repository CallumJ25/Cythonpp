#ifndef CYTHONPP_DOMAIN_SEMANTIC_TYPED_PRINTER_H
#define CYTHONPP_DOMAIN_SEMANTIC_TYPED_PRINTER_H

#include <string>

#include "domain/ast/module.h"
#include "type_map.h"

namespace cythonpp::domain::semantic {

// The test oracle for the type checker: `ast::AstPrinter`'s s-expression tree
// with `:type` appended wherever `TypeMap` has an entry for that node.
//
//   (Module
//     (AnnAssign (Name x) (Name int) (Constant 5):int)
//     (ExprStmt (BinOp + (Name x):int (Constant 1):int):int))
//
// That one rule -- suffix only where the map says so -- buys two things for
// free. Annotation subtrees (the second child of AnnAssign, a FunctionDef
// parameter's or return's annotation) render BARE with no special case,
// because TypeMap never holds an entry for an annotation expression -- see
// TypeMap's own comment for why. And a MISSING entry is visible in the
// oracle rather than silent: a test expecting `:int` and getting a bare node
// catches an untyped expression instead of passing by accident.
//
// IMPLEMENTATION CHOICE: this is a SIBLING Visitor that mirrors
// ast::AstPrinter's structure statement-for-statement and expression-for-
// expression, rather than a subclass of it. Subclassing was considered
// first, since the two renderers agree on every bracket and space, but
// AstPrinter exposes no way to reuse that agreement: `out_` and `depth_` are
// private with no protected accessor, so a derived class has no way to
// write a suffix into the buffer a base-class `visit` call just populated.
// The duplication this causes is real -- every statement-shaped `visit` here
// is a verbatim copy of AstPrinter's -- and a shared helper (e.g. AstPrinter
// exposing a protected `out_`/`depth_` or a `render(Node, Visitor&)`
// building block both renderers call through) would remove it cheaply if
// AstPrinter is ever revisited. Only the thirteen expression-node overloads
// differ from AstPrinter's, each by exactly one trailing call that appends
// the type suffix when TypeMap has an entry for that node.
class TypedPrinter {
public:
    std::string print(const ast::Module& module, const TypeMap& types);
};

} // namespace cythonpp::domain::semantic

#endif // CYTHONPP_DOMAIN_SEMANTIC_TYPED_PRINTER_H
