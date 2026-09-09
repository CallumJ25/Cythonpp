#ifndef CYTHONPP_DOMAIN_SEMANTIC_EXPRESSION_TYPER_H
#define CYTHONPP_DOMAIN_SEMANTIC_EXPRESSION_TYPER_H

#include <limits>
#include <string>

#include "class_table.h"
#include "domain/ast/attribute.h"
#include "domain/ast/bin_op.h"
#include "domain/ast/bool_op.h"
#include "domain/ast/call.h"
#include "domain/ast/compare.h"
#include "domain/ast/constant.h"
#include "domain/ast/dict_expr.h"
#include "domain/ast/expr.h"
#include "domain/ast/list_comp.h"
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
// three container displays -- ListExpr, DictExpr, TupleExpr (Task 13) --
// Subscript/Attribute (Task 14), Call (Task 15), and ListComp (Task 16) are
// real. Every other Expr kind returns Type::unknown() SILENTLY, with no
// report, so an intermediate build never emits a diagnostic a later task has
// to un-emit.
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

    // The ordering rule (Task 11, "the rule with teeth") needs the READING
    // statement's own line, which only the statement-level checker (Task 17)
    // knows -- so it calls this before typing each statement's subtree.
    // Left at its default (see statement_line_'s comment) for every existing
    // caller that never calls this, so the check is inert unless a caller
    // opts in.
    void set_statement_line(int line);

    // What iterating `iterable_type` (the ALREADY-typed iterable expression
    // `iterable_expr`) yields, reported through the SAME apply() switch
    // type_of_list_comp uses for its identical need (Task 16) -- so a `for`
    // loop's target (Task 20, TypeChecker) never carries a second, drifting
    // copy of the three-way RuleResult switch. Public (unlike apply itself)
    // because TypeChecker is not an ExpressionTyper and has no other way to
    // reach the shared rule table's Ok/NotApplicable/Unsupported handling.
    Type element_type_of(const ast::Expr& iterable_expr, const Type& iterable_type);

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
    // mypy-clean). That check is now recursive over the whole dotted chain,
    // so a qualified nested-class receiver (`Outer.Inner.x`, `A.B.C`) is
    // handled too -- see class_object_receiver.
    Type type_of_attribute(const ast::Attribute& attribute);

    // The qualified class name a receiver expression denotes as a CLASS
    // OBJECT -- "Outer" for `Outer`, "Outer.Inner" for `Outer.Inner` -- or an
    // empty string when the expression is not a chain of plain names ending
    // in a known class.
    //
    // Recursive over the chain, which is what makes nested classes work at
    // any depth. `x = Outer.Inner` and `A.B.C()` were both a false
    // `"Outer" has no attribute "Inner"` TypeError before this existed: the
    // one-segment version only recognised a bare-Name receiver, so the
    // SECOND segment fell into type_of_class_attribute, which looks only at
    // members and methods and has no notion of a nested class.
    //
    // PRECEDENCE is checked at the ROOT only, and it must stay there: a
    // local binding of the same name as a class wins (`def f(Widget: int)`
    // makes `Widget` an int parameter, not the class), so the root name is
    // rejected outright when scopes_ resolves it. Later segments are
    // attribute names, which no scope can bind.
    //
    // SIDE EFFECT, deliberately: every node in the chain it accepts gets its
    // TypeMap entry recorded here, by hand. That is the whole reason this
    // path exists -- typing those nodes through type_of() would consult
    // ScopeStack and report the false NameError -- so the recording cannot
    // be left to the caller.
    std::string class_object_receiver(const ast::Expr& expr);

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

    // `f(x)`, `C(x)`, `w.m(x)`, ... (Task 15). `Call::callee()` is one of
    // five shapes, resolved with THIS precedence for a Name callee -- a live
    // SCOPE BINDING wins first, then a builtin, then a user class (see
    // type_of_name_call's own comment: this is the OPPOSITE order from the
    // brief's "class, then builtin" wording, because builtin_class_table.h
    // seeds "range", "int", "list", "zip" and friends into ClassTable too --
    // checking is_class() before the builtin classifier made `range(3)` and
    // `zip(...)` resolve as zero-arg-constructor CLASSES instead, matching
    // this codebase's existing builtin_type_kind-before-is_class invariant,
    // not the brief's plain-English summary of it). A purely syntactic
    // class/builtin check would ALSO misread `def f(len: int): return
    // len(1)` as the builtin, which is why the scope check runs first of
    // all -- the same shape as type_of_attribute's class-object precedence
    // check.
    //
    // `expected` is threaded through ONLY for a supported builtin callee --
    // list()/dict()/set()/frozenset()/tuple() with zero arguments need it,
    // identical to an empty []/{} display (Task 13's empty-display rule,
    // corrected for Task 15: WITH a usable context, take it; WITHOUT one,
    // Unknown SILENTLY, never a report -- the var-annotated diagnostic
    // belongs to a later task's Assign arm). Every other row ignores it.
    Type type_of_call(const ast::Call& call, const Type& expected);

    // The Name-callee half of type_of_call, split out because it alone has
    // the three-way precedence (scope binding, then builtin, then class) and
    // needs `expected` for the builtin branch.
    Type type_of_name_call(const ast::Name& callee, const ast::Call& call, const Type& expected);

    // Checks `call.args()` POSITIONALLY against `callable`'s own parameter
    // types (args()[0..N-1], return LAST -- Type::callable's convention) and
    // yields the return type. `label` is how the callee is named in a
    // diagnostic -- `"f"` for a function or constructor, `"m" of "C"` for a
    // method -- already fully quoted, so callers just concatenate it into a
    // sentence.
    //
    // Every argument is typed FIRST, unconditionally, with its own parameter
    // type as `expected` when one exists (this is what makes `f([])` work
    // when f takes a list[int]) -- BEFORE the arity check, so an extra or
    // missing argument still gets every IN-RANGE argument's own subexpression
    // typed (and any root cause inside one, e.g. an unbound name, still
    // reported). An ARITY mismatch (too many or too few) reports ONCE and
    // returns early, WITHOUT also running the per-parameter type check below
    // it -- mirroring mypy, which does not pile a second "incompatible type"
    // diagnostic for the same call on top of an arity error. Only once arity
    // matches exactly does each argument get checked against its parameter
    // type via is_subtype (never operator==), each mismatch its own
    // diagnostic (N bad arguments is N diagnostics, matching the per-item
    // list/dict rule), numbered from the FIRST user argument -- self is never
    // counted or mentioned, because a method's `self` was already dropped by
    // type_of_class_attribute (or never added, for a constructor, since
    // ClassTable::constructor_type strips it) before this function ever
    // sees `callable`.
    Type type_of_positional_call(const Type& callable, const ast::Call& call,
                                 const std::string& label);

    // A constructor call whose arity and arguments are deliberately
    // unchecked -- see ClassTable::constructor_accepts_any_arity. Shared by
    // type_of_name_call's bare-`C()` branch and type_of_call's nested-class
    // `Outer.Inner()` branch, so the two can never drift apart. Types every
    // argument against Type::unknown() (one root cause inside an argument,
    // one diagnostic -- the same rule every other arm in this file follows)
    // and returns the instance type `constructor` itself already carries as
    // its return (constructor.args.back()), or Type::unknown() on the
    // unreachable-today empty-args shape type_of_positional_call also
    // guards against.
    Type type_of_unchecked_construction(const Type& constructor, const ast::Call& call);

    // The shared tail for every callee shape ONCE ITS OWN TYPE IS KNOWN --
    // a bound Name, an Attribute (already correctly self-bound or
    // self-unbound per THE self CONTRACT above), or anything else: Callable
    // dispatches to type_of_positional_call; Unknown absorbs silently (the
    // root cause already reported); Union reports NotImplementedError
    // (needs narrowing, exactly like every other operand arm's Union
    // handling -- a `Callable | None` callee might still be legal after
    // narrowing, so TypeError would risk a false positive); Class reports
    // NotImplementedError too, since `__call__` may be user-defined and this
    // model does not check for it (verified mypy-clean when it exists);
    // anything else reports TypeError "\"<kind>\" not callable". Every
    // argument is still typed against Type::unknown() in every non-Callable
    // case, so a root cause inside one still reports exactly once even
    // though there is no parameter list to check it against.
    Type type_of_call_result(const Type& callee_type, const ast::Call& call,
                             const std::string& label);

    // `[element for target in iterable if condition...]` (Task 16), the
    // ONLY expression that pushes a scope. Clause order: type the clause's
    // `iterable` in whatever scope is CURRENT (the enclosing scope for the
    // first clause, since the comprehension's own scope is not pushed yet;
    // the comprehension's own scope for every later clause, so a later
    // clause's iterable can read an earlier clause's target), take its
    // element_type, push the Comprehension scope on the first clause only,
    // bind `target` to the element type, type each `condition`. Once every
    // clause is processed, type `element` and wrap it in list_of.
    //
    // A tuple target (`[k for k, v in pairs]`) is mypy-clean but reported
    // here as NotImplementedError -- see the .cpp definition for why binding
    // element-wise from element_type's union would be wrong rather than
    // merely unsupported.
    //
    // The push is owned by a small RAII guard, not a paired push()/pop():
    // there are several report-and-return paths below (a non-iterable
    // iterable, a tuple target), and a bare pop() skipped by one of them
    // would corrupt every subsequent lookup in the file.
    Type type_of_list_comp(const ast::ListComp& list_comp);

    // The three-way switch, in one place. Every rule-table call goes through
    // this, which is what makes the false-TypeError path unreachable by
    // omission.
    Type apply(const RuleResult& result, const ast::Expr& at, std::string type_error_message);

    // The ONE place the "is not iterable" message
    // literal is spelled, shared by type_of_list_comp's own element_type
    // call and the public element_type_of (which For's TypeChecker arm
    // uses) -- previously each built the identical string inline, so this
    // is a genuine de-duplication, not just routing through apply().
    static std::string not_iterable_message(const Type& iterable_type);

    // Reports at `at`'s span start and returns Unknown, mirroring
    // AnnotationResolver::error, so every failure path is one line.
    Type error(const ast::Expr& at, std::string code, std::string message);

    ScopeStack& scopes_;
    const ClassTable& classes_;
    TypeMap& types_;
    diagnostics::DiagnosticSink& sink_;

    // The line of the statement currently being checked, for the ordering
    // rule in type_of_name. Defaults to INT_MAX -- not 0 -- so a caller that
    // never calls set_statement_line (every existing expression_typer_test.cpp
    // fixture, which types one bare expression with no enclosing statement)
    // gets a comparison that can never fire: a real declared_line is always
    // far smaller than INT_MAX. Defaulting to 0 would have made the ordering
    // check fire on EVERY own-scope binding for every caller that does not
    // opt in, since declared_line >= 0 is always true.
    int statement_line_ = std::numeric_limits<int>::max();
};

} // namespace cythonpp::domain::semantic

#endif // CYTHONPP_DOMAIN_SEMANTIC_EXPRESSION_TYPER_H
