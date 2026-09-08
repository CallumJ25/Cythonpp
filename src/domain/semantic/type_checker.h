#ifndef CYTHONPP_DOMAIN_SEMANTIC_TYPE_CHECKER_H
#define CYTHONPP_DOMAIN_SEMANTIC_TYPE_CHECKER_H

#include <map>
#include <set>
#include <string>
#include <vector>

#include "class_table.h"
#include "domain/ast/ann_assign.h"
#include "domain/ast/assign.h"
#include "domain/ast/attribute.h"
#include "domain/ast/class_def.h"
#include "domain/ast/expr.h"
#include "domain/ast/expr_stmt.h"
#include "domain/ast/function_def.h"
#include "domain/ast/module.h"
#include "domain/ast/name.h"
#include "domain/ast/node.h"
#include "domain/ast/recursive_visitor.h"
#include "domain/ast/subscript.h"
#include "domain/ast/tuple_expr.h"
#include "domain/diagnostics/diagnostic_sink.h"
#include "expression_typer.h"
#include "scope_stack.h"
#include "type.h"
#include "type_map.h"

namespace cythonpp::domain::semantic {

// The type-checking pass: `ast::Module` in, `TypeMap` out, `sink` gets every
// diagnostic on the way.
//
// A RecursiveVisitor, not a dynamic_cast dispatcher like AnnotationResolver
// or ExpressionTyper: a node this task does not handle should still have its
// children checked (RecursiveVisitor's default), so the worst case for an
// unimplemented statement is a missed rule, never a confidently wrong one.
//
// SCOPE (Task 17): Module, Assign, AnnAssign, ExprStmt, and the two-phase
// module-scope collection. Pass/Break/Continue carry nothing to check and are
// deliberately left un-overridden -- RecursiveVisitor's empty default is
// already correct for all three. ClassDef (Task 19) and control flow / Return
// (Task 20) are also left un-overridden for now, which is exactly the right
// intermediate behaviour: their children still get walked (and, for a body
// statement this task DOES handle, still checked), so the worst case is that
// a construct only Task 19-20 will add real rules for is silently
// under-checked rather than wrongly flagged.
//
// FunctionDef (Task 18) is now fully checked: every parameter (except a
// method's `self`) and the return both need an annotation, a wrong-typed
// default is reported at the `def` line, the function's own name is bound
// before its body is checked (so direct recursion works), and a NESTED def
// is bound at its lexical position -- no hoisting -- via the SAME
// placeholder-then-fill pattern pre_bind_assignment_targets/assign_name use
// at module scope, extended in pre_bind_function_body to also cover a
// nested def's own name and a nested AnnAssign target (bind_annotation grew
// the matching "own still-unfilled placeholder" case to support it).
//
// ClassDef is overridden ONLY to track whether a FunctionDef sits directly
// in a class body -- self-exemption and the __init__ carve-out both need
// that, and nothing else currently does -- via in_class_body_, a plain bool
// rather than a ScopeKind::Class push. Task 19 owns the real Class scope,
// the order-sensitive class body, and attribute collection; this override
// pushes NOTHING onto ScopeStack and declares NOTHING into ClassTable, so
// it changes no existing behaviour of its own. A method's name is never
// bound into ScopeStack (ClassTable is Task 19's sole source of truth for
// method names, exactly like a class's own name).
class TypeChecker : public ast::RecursiveVisitor {
public:
    explicit TypeChecker(diagnostics::DiagnosticSink& sink);

    // Total and non-throwing. Runs the two-phase module-scope collection
    // (see visit(Module&)), then checks the body in source order, and
    // returns the TypeMap ExpressionTyper populated on the way.
    TypeMap check(const ast::Module& module);

    void visit(const ast::Module& node) override;
    void visit(const ast::Assign& node) override;
    void visit(const ast::AnnAssign& node) override;
    void visit(const ast::ExprStmt& node) override;
    void visit(const ast::FunctionDef& node) override;
    void visit(const ast::ClassDef& node) override;

private:
    // What resolving (and possibly binding) an AnnAssign's annotation
    // produced -- shared by Phase 2's module-level pre-bind and this task's
    // own fallback for a NESTED AnnAssign (e.g. inside a class body), which
    // Phase 2 never sees because it only scans module.body() directly.
    struct AnnotationBinding {
        Type type;
        // True when the target name was already bound in the current scope,
        // so binding was refused and a redefinition was already reported --
        // in that case the value must still be typed (for the TypeMap) but
        // never checked for compatibility, since mypy reports the
        // redefinition ALONE, not an assignment error alongside it.
        bool redefinition = false;
    };

    // PRE-PASS, in source order: every top-level ClassDef/FunctionDef name,
    // recording the FIRST line it was declared at. A SECOND top-level
    // ClassDef/FunctionDef under an already-recorded name is a redefinition,
    // reported here (against the true first occurrence, regardless of which
    // phase would otherwise touch that node first) and remembered in
    // collided_top_level_ so Phase 2 skips re-processing it.
    //
    // This is the ONE place a class and a def sharing a name are compared:
    // a class's name lives only in ClassTable, never in ScopeStack (binding
    // it there would invert the class-object-receiver precedence check in
    // ExpressionTyper's Attribute/Call arms), so ScopeStack::bind's own
    // built-in collision detection -- which Phase 2 relies on for a pure
    // def/def or def/AnnAssign collision -- can never see a class name to
    // compare against.
    void scan_top_level_names(const ast::Module& module);

    // Phase 1: declare every top-level ClassDef's name and bases into
    // ClassTable (no annotation resolution yet -- a base may name a class
    // declared later in the same module), THEN -- once every class is
    // declared -- validate that each bare-Name base actually resolves,
    // reporting NameError for one that does not (e.g. `class C(Generic):`,
    // since Generic cannot be imported in this subset).
    void collect_classes(const ast::Module& module);

    // Phase 2: resolve every top-level FunctionDef signature and every
    // module-level AnnAssign's annotation, binding each name into ScopeStack
    // with its declaration line -- so a function body (once Task 18 checks
    // one) or a later statement can see a class declared below it (Phase 1
    // already ran) or a name declared below it in source order without a
    // false NameError once Task 18 wires up FunctionDef bodies.
    void collect_signatures(const ast::Module& module);

    // Phase 2.5: every top-level Assign's target name(s) that are not yet
    // bound (i.e. not a FunctionDef/AnnAssign name from Phase 2) get a
    // PLACEHOLDER binding -- Type::unknown(), at the statement's own line --
    // so a module-level "used before definition" read (`y = x` before
    // `x = 5`) resolves to a real Binding whose declared_line lets
    // ExpressionTyper's ordering check fire with the right wording, rather
    // than falling through to "not defined". Phase 3's own Assign handling
    // recognises "my own placeholder, still unfilled" by comparing this
    // Binding's declared_line to the statement it is currently checking, and
    // replaces it (via ScopeStack::rebind) with the real inferred type
    // exactly once -- see assign_name.
    void pre_bind_assignment_targets(const ast::Module& module);
    void pre_bind_target(const ast::Expr& target, int line);

    // The Function-scope analogue of pre_bind_assignment_targets, run once a
    // FunctionDef's own Function scope is current and its parameters are
    // bound, over that SAME FunctionDef's own body list directly (not
    // recursively into a nested block, matching pre_bind_assignment_targets'
    // own module.body()-only scope). Task 18's twist, absent at module
    // scope: a Function scope gets no Phase-2 equivalent AT ALL, so BOTH an
    // Assign target AND a nested def's own name need a placeholder here --
    // a nested `def` is bound at its lexical position, never hoisted, but
    // the ordering check still needs a Binding to exist (even an Unknown
    // one) before the def's own line is reached, or an early same-scope read
    // would report "not defined" instead of "used before definition". A
    // nested AnnAssign target is placeholder-bound too, for the identical
    // reason (see bind_annotation's own "still-unfilled placeholder" case,
    // added alongside this) -- module scope needs no such placeholder for
    // AnnAssign because collect_signatures's Phase 2 already binds every
    // module-level AnnAssign with its REAL resolved type ahead of time, an
    // eager pass this function deliberately does not attempt to replicate
    // (that would re-invoke AnnotationResolver on the same annotation twice,
    // once here and once when the statement is actually visited).
    void pre_bind_function_body(const std::vector<ast::StmtPtr>& body);

    // Extracts a base's name for ClassTable::declare. Only a bare Name is
    // handled -- a subscripted or attribute base (`Generic[T]`, `a.B`) is
    // outside this task's tested scope and is simply omitted, which only
    // matters once something walks the base chain looking for it.
    static std::vector<std::string> base_names(const std::vector<ast::ExprPtr>& bases);

    // Resolves `annotation` and attempts to bind `target` into the CURRENT
    // scope with it (annotated = true). If the name is already bound there,
    // reports the settled redefinition wording and returns
    // {type, redefinition = true} without touching the existing binding --
    // the first declaration wins, matching ScopeStack::bind's own contract.
    AnnotationBinding bind_annotation(const ast::Name& target, const ast::Expr& annotation,
                                      int line);

    // Assign, dispatched by target shape.
    void assign_to(const ast::Expr& target, const ast::Expr& value, int line);
    void assign_tuple(const ast::TupleExpr& target, const ast::Expr& value, int line);
    void assign_subscript(const ast::Subscript& target, const ast::Expr& value);
    void assign_attribute(const ast::Attribute& target, const ast::Expr& value);

    // The one place a Name target is bound or checked, for both a plain
    // Assign and each element of a tuple-unpacking Assign. See
    // pre_bind_assignment_targets for what "my own still-unfilled
    // placeholder" means and why declared_line == line is the signal for it.
    void assign_name(const ast::Name& target, const Type& value_type, int line);

    // True for `[]`, `{}`, or a zero-argument call to
    // list/dict/set/frozenset/tuple -- the five constructs mypy leaves
    // silently un-annotated (ExpressionTyper returns Unknown for them with NO
    // report), so this is the only place the "need type annotation" error
    // can come from, and the only place with the variable's name to put in
    // it. Purely SYNTACTIC and independent of the value's inferred type, so
    // it never misfires on an unrelated Unknown (e.g. `x = nope`, where the
    // Unknown came from an already-reported NameError). A bare `()`
    // (TupleExpr) is deliberately excluded: tuple[()] is a complete,
    // non-generic type needing no annotation, unlike a bare call to
    // `tuple()`, which mypy leaves just as unannotated as `[]`.
    static bool is_bare_empty_container(const ast::Expr& value);

    void report(const ast::Node& at, std::string code, std::string message);
    void report_incompatible_assignment(const ast::Node& at, const Type& value_type,
                                        const Type& target_type, const char* target_label);

    diagnostics::DiagnosticSink& sink_;
    ScopeStack scopes_;
    ClassTable classes_;
    TypeMap types_;
    ExpressionTyper typer_;

    // Populated by scan_top_level_names; see its comment.
    std::map<std::string, int> top_level_lines_;
    std::set<const ast::Node*> collided_top_level_;

    // Phase 2's resolution for every module-level AnnAssign, keyed by node
    // address so Phase 3's visit(AnnAssign&) sees the SAME resolution rather
    // than calling AnnotationResolver a second time -- doing so twice would
    // double-report a bad annotation.
    std::map<const ast::AnnAssign*, AnnotationBinding> module_level_annotations_;

    // Phase 2's resolved Callable for every top-level FunctionDef collect_
    // signatures actually processed (i.e. NOT one collided_top_level_
    // skipped), keyed by node address -- Task 18's visit(FunctionDef) reuses
    // it exactly like module_level_annotations_ above, so a top-level def's
    // parameter/return annotations are resolved through AnnotationResolver
    // exactly ONCE. args()[0..N-1] are the parameter types, args().back()
    // the return type, per Type::callable's own "return last" convention --
    // deliberately reusing that shape instead of a bespoke struct. A def
    // collect_signatures skipped (collided_top_level_) or never saw at all
    // (a NESTED def, or a method) has no entry here and is resolved fresh,
    // directly in visit(FunctionDef), the only time it is ever resolved.
    std::map<const ast::FunctionDef*, Type> top_level_signatures_;

    // Set only by visit(ClassDef) around walking that class's OWN body list,
    // and reset to false for the duration of a FunctionDef's own body (a
    // method's nested def is not itself a method) -- see the class-level
    // comment. False at every point outside a class body statement list,
    // including the outermost module scope.
    bool in_class_body_ = false;
};

} // namespace cythonpp::domain::semantic

#endif // CYTHONPP_DOMAIN_SEMANTIC_TYPE_CHECKER_H
