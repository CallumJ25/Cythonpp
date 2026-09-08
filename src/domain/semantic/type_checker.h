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
#include "domain/ast/for.h"
#include "domain/ast/function_def.h"
#include "domain/ast/if.h"
#include "domain/ast/module.h"
#include "domain/ast/name.h"
#include "domain/ast/node.h"
#include "domain/ast/recursive_visitor.h"
#include "domain/ast/return.h"
#include "domain/ast/subscript.h"
#include "domain/ast/tuple_expr.h"
#include "domain/ast/while.h"
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
// already correct for all three.
//
// Control flow and Return (Task 20) are now real. If/While type their
// condition (ANY type is fine -- truthiness is universal) then walk body()
// and orelse() in that order; neither pushes a scope, matching Python's own
// lack of block scoping. For types iterable() and takes its element type
// through ExpressionTyper::element_type_of (the SAME apply() switch
// type_of_list_comp already uses -- see that method's own comment -- so this
// is not a second, drifting copy of the three-way RuleResult handling), then
// binds target() via assign_name in the CURRENT scope (a for target does NOT
// get its own scope, unlike a comprehension -- the target survives the loop),
// then walks body()/orelse() the same way. A TupleExpr target reports
// NotImplementedError ("tuple targets in for loops are not supported")
// rather than TypeError: mypy accepts one, and element_type of a tuple[K, V]
// is the union K | V, not a positional pair, so there is nothing correct to
// bind element-wise.
//
// Return's value (if any) is typed with the ENCLOSING function's declared
// return type as expected context -- current_return_type_, pushed by a small
// RAII guard around FunctionDef's own body walk (see visit(FunctionDef)) and
// restored on the way out, exactly like ClassContextGuard restores
// current_class_qualified_name_ -- so `def f() -> list[int]: return []`
// checks the bare `[]` against list[int] instead of Unknown. Unknown here
// means "no reliable declared type" (either no return annotation at all, or
// one that failed to resolve) and is treated as absorbing throughout: a bare
// return or a value return under it is never flagged, matching mypy's own
// silence on an untyped def's return statements (the SEPARATE "function is
// missing a type annotation" diagnostic already covers that def). A bare
// `return` in a function whose declared return type is neither None nor
// Unknown is "return value expected"; a value `return` where the declared
// type IS None is "no return value expected"; an incompatible value is
// "incompatible return value type (got \"...\", expected \"...\")" -- all
// three verified against mypy 1.18.1's own (title-cased) wording, lower-cased
// to match this codebase's existing message casing convention.
//
// RETURN-PATH CHECKING, the sole flow-sensitive check (mypy runs it despite
// declining definite-assignment analysis generally): `always_returns` is a
// purely syntactic, non-recursive-into-nested-scopes walk over a statement
// list -- a Return is a hit; an If counts only when orelse() is NON-EMPTY and
// BOTH branches always return; a While counts only when its condition is the
// literal `True` (a Constant whose token type is BOOL_TRUE) AND its body has
// no reachable break (see contains_reachable_break -- a break belonging to a
// NESTED For/While does not count, since it can never escape THIS loop); a
// For, or a While with any other condition, is always assumed skippable
// (false) -- both directions verified against mypy, and the only possible
// error from the approximation is a MISSED one (mypy says "definitely
// returns", this checker says "maybe not"), never a false positive. Checked
// once per FunctionDef, at the very end of its body walk, ONLY when the
// function has a return annotation that is neither None nor Unknown --
// reported at the `def` line as TypeError "missing return statement" (mypy
// splits this one message across two codes, empty-body and return; this
// checker does not distinguish them).
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
// ClassDef (Task 19) is now fully checked: a class body is a REAL
// ScopeKind::Class push (current_class_qualified_name_ tracks the qualified
// name for the DURATION of that push, restored by ClassContextGuard, so
// nested classes declare under "Outer.Inner" and self inside one of Inner's
// methods binds to Class("Outer.Inner"), never to Outer's). "Is this
// FunctionDef a method" collapsed entirely into
// `scopes_.current_kind() == ScopeKind::Class`, checked BEFORE
// FunctionScopeGuard pushes the Function scope -- no separate bool needed
// (a prior, minimal ClassDef override tracked one, in_class_body_, purely for
// this question; Task 19 replaces it outright). A method's name is never
// bound into ScopeStack (ClassTable is the sole source of truth for both
// method and class names -- see the class-object-receiver precedence hazard
// documented on ExpressionTyper::type_of_attribute); its SIGNATURE (self
// included, per ClassTable::method_type's own contract) is declared into
// ClassTable via declare_method as soon as it is known, which is also what
// makes __init__ discoverable as a constructor. self itself is bound to
// Class(current_class_qualified_name_) instead of Unknown -- see
// assign_attribute for why this cannot land without ALSO collecting
// attributes in the same change (self.x would otherwise become a false
// attr-defined TypeError the moment self stops being the absorbing Unknown).
//
// Attributes come from two places, both closing the attribute set at
// declaration time (an assignment to an attribute the class never declared,
// from OUTSIDE the class, is attr-defined -- see assign_attribute's ordinary
// path, unchanged from Task 17):
//   - A class-body AnnAssign (visit(AnnAssign), when the CURRENT scope is
//     Class at the time it runs -- true for one directly in the body, and
//     for one nested in an if/for inside it too, since Python itself does
//     not scope those) also calls ClassTable::declare_member, in addition to
//     the ordinary scope-bind bind_annotation already performs. Declared
//     whether or not the AnnAssign carries a value -- verified mypy accepts
//     `C.x` for a bare `x: int` class-body annotation.
//   - `self.x = ...` inside ANY method (not just __init__) -- handled in
//     assign_attribute, checked BEFORE the ordinary read path so a brand-new
//     attribute is not a false attr-defined miss. The FIRST such assignment
//     TypeChecker's own visitation order encounters declares the member (its
//     type inferred from the value, exactly like an ordinary Name
//     assignment); this is single-pass, not a separate collect phase, so it
//     matches every VERIFIED test in the corpus (every one either declares
//     from a single method or declares-then-conflicts in textual method
//     order) but does NOT handle a method appearing BEFORE the one that
//     first assigns an attribute it reads via self -- an out-of-order
//     forward reference across two methods' bodies, untested here and left
//     for a future task if it turns out to matter.
//
// collect_classes (Phase 1) now RECURSES into every class body to declare a
// NESTED ClassDef under its qualified name too, before Phase 2 resolves any
// annotation -- `x: Outer.Inner` needs "Outer.Inner" declared in ClassTable
// by the time collect_signatures reaches it, and Phase 3 (the ordinary
// per-statement walk, which is what would otherwise declare Inner) does not
// run until after Phase 2 finishes. It also now SKIPS a collided top-level
// ClassDef (scan_top_level_names already reported it) instead of declaring
// it anyway -- previously the LOSING class's declare() call silently
// overwrote the winning one in ClassTable, so a later use resolved against
// the wrong (reported-as-erroneous) class's bases/members; collect_signatures
// already had the matching skip for a colliding FunctionDef, so this was an
// asymmetry, not a deliberate choice.
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
    void visit(const ast::If& node) override;
    void visit(const ast::While& node) override;
    void visit(const ast::For& node) override;
    void visit(const ast::Return& node) override;

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
    // declared later in the same module), RECURSING into each class's own
    // body to declare a NESTED ClassDef too, under its qualified name (see
    // declare_class_recursive) -- so `x: Outer.Inner` resolves in Phase 2,
    // which runs before Phase 3 (the ordinary walk) ever reaches Inner's own
    // ClassDef node. A top-level ClassDef scan_top_level_names already
    // reported as a collided redefinition is SKIPPED here (Task 19 fix: it
    // used to be declared anyway, silently overwriting the winning
    // same-named class's ClassTable entry). THEN -- once every class is
    // declared -- validate that each bare-Name base actually resolves,
    // reporting NameError for one that does not (e.g. `class C(Generic):`,
    // since Generic cannot be imported in this subset).
    void collect_classes(const ast::Module& module);

    // The recursive half of collect_classes: declares `class_def` under
    // `qualified_prefix + "." + class_def.name()` (or just its own name, at
    // the top level, where `qualified_prefix` is empty), appends it to
    // `all_classes` for the base-validation loop collect_classes runs once
    // every class -- at every nesting depth -- is declared, then recurses
    // into `class_def`'s own body for a nested ClassDef, passing ITS OWN
    // qualified name down as the next prefix.
    void declare_class_recursive(const ast::ClassDef& class_def, const std::string& qualified_prefix,
                                 std::vector<const ast::ClassDef*>& all_classes);

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

    // Task 19: `self.x = ...` inside a method DECLARES a new instance
    // attribute the first time TypeChecker's own single-pass visitation
    // encounters it for a given name -- checked FIRST, syntactically plus one
    // ScopeStack::resolve (never typed, so this check alone cannot itself
    // report anything): the receiver is a bare Name spelled "self" AND it
    // currently resolves to Class(current_class_qualified_name_). Only once
    // ClassTable confirms the member/method does not already exist (from an
    // earlier assignment in THIS class, or inherited from a base) does this
    // take the declare-a-new-member path, inferring the type from the value
    // exactly like an ordinary Name assignment and calling
    // ClassTable::declare_member with the ASSIGNMENT's own line. Every other
    // shape -- an attribute store from outside the class, a conflicting
    // SECOND self.x assignment once the member already exists, a `self` that
    // is not really bound to the enclosing class (shadowed, or outside any
    // method) -- falls through to the ordinary read-then-compare path
    // unchanged from Task 17, which is what makes a later conflicting
    // self.x assignment a TypeError (first assignment's type is sticky, same
    // rule as assign_name) and an attribute store from OUTSIDE the class a
    // genuine attr-defined TypeError (the set is closed there).
    void assign_attribute(const ast::Attribute& target, const ast::Expr& value);

    // The one place a Name target is bound or checked, for both a plain
    // Assign and each element of a tuple-unpacking Assign. See
    // pre_bind_assignment_targets for what "my own still-unfilled
    // placeholder" means and why declared_line == line is the signal for it.
    void assign_name(const ast::Name& target, const Type& value_type, int line);

    // Task 18 fix round 1, Finding 1: true when `binding` is THIS exact
    // statement's own still-unfilled placeholder (from
    // pre_bind_assignment_targets / pre_bind_function_body) rather than a
    // genuine prior binding -- the signal being declared_line == line, AS
    // LONG AS the binding is not order_exempt. A parameter's declared_line
    // is the `def` line, which for a one-line suite equals the body
    // statement's own line too, but a parameter is never a placeholder to
    // fill in -- it already carries its real (possibly annotated) type --
    // so order_exempt vetoes the match. Shared by assign_to (for both the
    // bidirectional `expected` type and the bare-empty-container check) and
    // assign_name, so the parameter exemption cannot be added to one call
    // site and missed on another.
    static bool is_unfilled_placeholder(const Binding& binding, int line);

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

    // Task 20's return-path check. Purely syntactic and const -- it touches
    // no scope, no ClassTable, nothing but the AST shape -- so it can be
    // (and is) called after the function's own body has already been
    // visited, with no ordering hazard either way. See the class-level
    // comment for the exact per-statement rule; "a body always returns if
    // ANY of its statements does" is the fold this recursion performs at
    // every level, mirroring collect_classes' own recursive-then-fold shape.
    bool always_returns(const std::vector<ast::StmtPtr>& body) const;

    // The `while True` arm's "no reachable break" half: true when `body`
    // contains a `break` at any depth EXCEPT inside a nested For/While's own
    // body/orelse -- a break belonging to a nested loop can only ever escape
    // THAT loop, never this one, so recursing into one would over-count.
    // Recurses into If's body/orelse (an `if` is not a loop, so a break
    // inside one still belongs to the enclosing loop), which is the one
    // compound statement this helper DOES look through.
    static bool contains_reachable_break(const std::vector<ast::StmtPtr>& body);

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

    // The QUALIFIED name of the class whose body is currently being walked --
    // "Outer.Inner" while inside Inner's own body, restored to whatever it
    // was (by ClassContextGuard, in the .cpp) once that body's walk is done.
    // Empty at every point outside a class body statement list, including
    // the outermost module scope -- which is also what
    // `scopes_.current_kind() == ScopeKind::Class` means now, replacing the
    // old in_class_body_ bool entirely (see the class-level comment): a
    // FunctionDef checks that scope-kind test, BEFORE its own Function scope
    // is pushed, to decide "is this a method", and a method's nested def
    // sees ScopeKind::Function instead (already pushed by its own enclosing
    // method's FunctionScopeGuard) with no separate reset ever needed.
    std::string current_class_qualified_name_;

    // The CURRENT function's declared return type, for Return's own checks --
    // Type::unknown() outside any function, and restored to whatever it was
    // (by ReturnContextGuard, in the .cpp) once that function's body walk is
    // done, so a nested def's own return statements are checked against ITS
    // OWN return type, never the enclosing one's. Unknown means "no reliable
    // declared type" (no annotation at all, or one that failed to resolve),
    // which Return treats as absorbing -- consistent with every other Unknown
    // in this checker, and matching mypy's own silence on an untyped def's
    // return statements (a SEPARATE diagnostic already flags the missing
    // annotation itself).
    Type current_return_type_ = Type::unknown();
};

} // namespace cythonpp::domain::semantic

#endif // CYTHONPP_DOMAIN_SEMANTIC_TYPE_CHECKER_H
