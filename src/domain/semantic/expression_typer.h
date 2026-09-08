#ifndef CYTHONPP_DOMAIN_SEMANTIC_EXPRESSION_TYPER_H
#define CYTHONPP_DOMAIN_SEMANTIC_EXPRESSION_TYPER_H

#include <string>

#include "class_table.h"
#include "domain/ast/attribute.h"
#include "domain/ast/bin_op.h"
#include "domain/ast/bool_op.h"
#include "domain/ast/compare.h"
#include "domain/ast/constant.h"
#include "domain/ast/dict_expr.h"
#include "domain/ast/expr.h"
#include "domain/ast/list_expr.h"
#include "domain/ast/name.h"
#include "domain/ast/subscript.h"
#include "domain/ast/tuple_expr.h"
#include "domain/ast/unary_op.h"
#include "domain/diagnostics/diagnostic_sink.h"
#include "rule_result.h"
#include "scope_stack.h"
#include "type.h"
#include "type_map.h"

namespace cythonpp::domain::semantic {

// Gives every expression a Type, recording each one in `types` on the way.
//
// SCOPE: Constant, Name, UnaryOp, BinOp, Compare, BoolOp (Task 12), the
// three container displays -- ListExpr, DictExpr, TupleExpr (Task 13) -- and
// Subscript/Attribute (Task 14) are real. Every other Expr kind -- Call
// (Task 15), ListComp (Task 16), ... -- returns Type::unknown() SILENTLY,
// with no report, so an intermediate build never emits a diagnostic a later
// task has to un-emit.
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
    // `expected` is the bidirectional-checking context container displays
    // need it: `[]` has no element type without one. Type::unknown() means
    // "no context".
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

    // Container displays (Task 13). Each takes `expected` -- the enclosing
    // context, e.g. an AnnAssign's declared type, or a recursive call from an
    // outer display's own context branch -- because the two algorithms below
    // are genuinely different, not one with a fallback:
    //
    //   WITH a context of the matching kind (and, for TupleExpr, the
    //   matching arity): each element/entry is checked individually against
    //   the declared element type(s). For ListExpr/DictExpr, the result is
    //   the DECLARED type (`expected` itself) -- no join is computed -- and a
    //   mismatching element reports its own TypeError naming its
    //   index/position; every element is still visited, so N bad elements are
    //   N diagnostics, not one. TupleExpr is the ONE exception to "a
    //   mismatching element reports": it never reports here, even with a
    //   matching context. mypy has no per-item tuple diagnostic -- both an
    //   element mismatch (`x: tuple[int, str] = (1, 2)`) and an arity
    //   mismatch (`x: tuple[int, str] = (1,)`) surface as a single
    //   `assignment` error naming the two whole tuple types, produced by a
    //   later task's assignment check from the POSITIONAL type this arm
    //   returns (see type_of_tuple). `element_expected` still propagates into
    //   the recursive type_of() call either way, since that context
    //   propagation is independent of whether anyone reports here.
    //
    //   WITHOUT a usable context (Type::unknown(), the wrong TypeKind, or --
    //   for TupleExpr -- the wrong arity): elements are typed against
    //   Type::unknown() and folded with join() (Task 10), which is NOT
    //   recursive into invariant type arguments and is left-biased for
    //   multiple inheritance; TupleExpr has no join branch at all, since a
    //   tuple's element types are positional, not homogeneous, so it always
    //   builds tuple_of from each element's own inferred type.
    //
    // An empty ListExpr/DictExpr with no usable context returns
    // Type::unknown() SILENTLY -- mypy types a bare `[]` in expression
    // position as list[Never] and says nothing; the var-annotated
    // "need type annotation" error belongs to a later task's Assign/AnnAssign
    // arm, which is the only place the variable's name exists to put in the
    // message. An empty TupleExpr (`()`) is NOT silent Unknown: tuple[()] is
    // a complete, non-generic type needing no annotation, so it is built the
    // same way a non-empty tuple is, just with zero elements.
    Type type_of_list(const ast::ListExpr& list, const Type& expected);
    Type type_of_dict(const ast::DictExpr& dict, const Type& expected);
    Type type_of_tuple(const ast::TupleExpr& tuple, const Type& expected);

    // `container[index]` (Task 14). Delegates entirely to subscript_result --
    // every interesting row (bytes[int] -> int, a heterogeneous tuple's
    // union, a dict key checked by assignability) already lives in that rule
    // table -- and routes the three-way answer through apply().
    Type type_of_subscript(const ast::Subscript& subscript);

    // `value.attribute` (Task 14). Where the class table earns its keep: a
    // Class receiver is looked up in ClassTable, everything else (a builtin
    // kind, a Union needing narrowing, or Unknown) is handled without ever
    // consulting the class table. See type_of_class_attribute for the
    // Class-receiver cases.
    //
    // CLASS-OBJECT RECEIVER, checked FIRST and syntactically, before the
    // receiver expression is ever typed: `C.x` / `C.m` have a bare ast::Name
    // receiver whose identifier names a class itself (classes_.is_class(...)
    // is true). Nothing binds a class's own name into ScopeStack -- the
    // not-yet-written statement checker never does -- so routing `C` through
    // the ordinary type_of()/type_of_name() path would report a false
    // NameError on mypy-clean code (verified: `C.x` and `C.m` are both
    // mypy-clean). A qualified nested-class receiver (`Outer.Inner.x`) is out
    // of scope: its own receiver is an Attribute, not a Name, so it falls
    // through to the ordinary path and reports rather than guesses.
    Type type_of_attribute(const ast::Attribute& attribute);

    // The Class-receiver half of type_of_attribute, split out because it
    // alone has more than one case: a member (declared or inherited) wins,
    // then a method (a SEPARATE ClassTable query -- see class_table.h), then
    // a class-defined __getattr__ fallback, then the builtin-inheriting
    // carve-out, then a genuine attr-defined TypeError.
    //
    // `bind_self` is THE self CONTRACT, verified against mypy 1.18.1:
    //   - true  (an INSTANCE receiver, `c.m`): a resolved method's signature
    //     has args[0] (self) DROPPED here -- reveal_type(c.m) is
    //     `def () -> int`. Binding happens at THIS attribute access, not at
    //     a later call.
    //   - false (a CLASS-OBJECT receiver, `C.m`): the signature is returned
    //     UNCHANGED, self included -- reveal_type(C.m) is
    //     `def (self: C) -> int`.
    // Task 15's Call arm must NOT drop args[0] again for an Attribute
    // callee: an instance-bound method is already bound by the time the Call
    // arm sees it, and a class-object one is deliberately left unbound. A
    // double-drop is a silent arity bug.
    Type type_of_class_attribute(const Type& receiver, const ast::Attribute& attribute,
                                 bool bind_self);

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
