#ifndef CYTHONPP_DOMAIN_SEMANTIC_EXPRESSION_TYPER_H
#define CYTHONPP_DOMAIN_SEMANTIC_EXPRESSION_TYPER_H

#include <string>

#include "class_table.h"
#include "domain/ast/bin_op.h"
#include "domain/ast/bool_op.h"
#include "domain/ast/compare.h"
#include "domain/ast/constant.h"
#include "domain/ast/expr.h"
#include "domain/ast/name.h"
#include "domain/ast/unary_op.h"
#include "domain/diagnostics/diagnostic_sink.h"
#include "rule_result.h"
#include "scope_stack.h"
#include "type.h"
#include "type_map.h"

namespace cythonpp::domain::semantic {

// Gives every expression a Type, recording each one in `types` on the way.
//
// SCOPE OF THIS TASK (12): only the Constant, Name, UnaryOp, BinOp, Compare
// and BoolOp arms are real. Every other Expr kind -- container displays
// (Task 13), Subscript/Attribute (Task 14), Call (Task 15), ListComp
// (Task 16), ... -- returns Type::unknown() SILENTLY, with no report, so an
// intermediate build never emits a diagnostic a later task has to un-emit.
//
// Dispatch is dynamic_cast, following domain/parser/assignability.cpp and
// annotation_resolver.cpp, not a Visitor: a Visitor::visit returns void and
// takes one argument, so it cannot return a Type or thread `expected` through
// without smuggling both through member variables.
class ExpressionTyper {
public:
    ExpressionTyper(ScopeStack& scopes, const ClassTable& classes, TypeMap& types,
                    diagnostics::DiagnosticSink& sink);

    // Total and non-throwing. Returns Unknown after reporting, so callers need
    // no null check and no try/catch -- the same contract as
    // AnnotationResolver::resolve.
    //
    // `expected` is the bidirectional-checking context for a later task's
    // arms (container displays need it: `[]` has no element type without
    // one). Type::unknown() means "no context". Unused by this task's arms.
    Type type_of(const ast::Expr& expr, const Type& expected);

private:
    // The sign lives in the UnaryOp, not the lexeme -- `negated` is true only
    // when this Constant is the LITERAL_INT operand of a UnaryOp(-), the one
    // case where a magnitude of exactly 2^63 is representable (-2^63).
    Type type_of_constant(const ast::Constant& constant, bool negated);

    Type type_of_name(const ast::Name& name);
    Type type_of_unary_op(const ast::UnaryOp& unary);
    Type type_of_bin_op(const ast::BinOp& bin_op);
    Type type_of_compare(const ast::Compare& compare);
    Type type_of_bool_op(const ast::BoolOp& bool_op);

    // The three-way switch, in one place. Every rule-table call goes through
    // this, which is what makes the false-TypeError path unreachable by
    // omission.
    Type apply(const RuleResult& result, const ast::Expr& at, std::string type_error_message);

    // Reports at `at`'s span start and returns Unknown, mirroring
    // AnnotationResolver::error, so every failure path is one line.
    Type error(const ast::Expr& at, std::string code, std::string message);

    ScopeStack& scopes_;
    const ClassTable& classes_;
    TypeMap& types_;
    diagnostics::DiagnosticSink& sink_;
};

} // namespace cythonpp::domain::semantic

#endif // CYTHONPP_DOMAIN_SEMANTIC_EXPRESSION_TYPER_H
