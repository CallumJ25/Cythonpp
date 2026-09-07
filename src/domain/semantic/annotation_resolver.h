#ifndef CYTHONPP_DOMAIN_SEMANTIC_ANNOTATION_RESOLVER_H
#define CYTHONPP_DOMAIN_SEMANTIC_ANNOTATION_RESOLVER_H

#include <string>

#include "class_lookup.h"
#include "domain/ast/bin_op.h"
#include "domain/ast/constant.h"
#include "domain/ast/expr.h"
#include "domain/ast/name.h"
#include "domain/ast/subscript.h"
#include "domain/diagnostics/diagnostic_sink.h"
#include "type.h"

namespace cythonpp::domain::semantic {

// Turns an annotation Expr subtree into a Type.
//
// This is the component that discharges Spec 2's deferral: annotations are
// stored as ordinary Expr subtrees because they are syntactically
// expressions, and turning `Subscript(Name dict, TupleExpr(Name str, Name
// int))` into dict[str, int] is what this does.
//
// The only component in Spec 5a that reports. Total and non-throwing, like
// every stage before it: an annotation it cannot make sense of is a
// diagnostic on the sink and a Type::unknown() return, so callers need no
// null check and no try/catch.
//
// ONE DIAGNOSTIC PER FAILING SUB-ANNOTATION, and no cascade. Two undefined
// names inside `dict[Foo, Bar]` are two root causes and draw two
// diagnostics, as mypy's do; what never happens is a THIRD report from the
// enclosing dict, because a failed argument makes the whole annotation
// Unknown silently.
class AnnotationResolver {
public:
    AnnotationResolver(const ClassLookup& classes, diagnostics::DiagnosticSink& sink);

    Type resolve(const ast::Expr& annotation);

private:
    Type resolve_name(const ast::Name& name);
    Type resolve_constant(const ast::Constant& constant);
    Type resolve_subscript(const ast::Subscript& subscript);
    Type resolve_union(const ast::BinOp& operation);

    // Reports at `at`'s span start -- Diagnostic is a point, not a range, and
    // the spec declined to reopen that -- and returns Unknown so every
    // failure path is one line.
    Type error(const ast::Expr& at, std::string code, std::string message);

    const ClassLookup& classes_;
    diagnostics::DiagnosticSink& sink_;
};

} // namespace cythonpp::domain::semantic

#endif // CYTHONPP_DOMAIN_SEMANTIC_ANNOTATION_RESOLVER_H
