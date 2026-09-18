#ifndef CYTHONPP_DOMAIN_SEMANTIC_TYPE_CHECKER_H
#define CYTHONPP_DOMAIN_SEMANTIC_TYPE_CHECKER_H

#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "class_table.h"
#include "diagnostic_kind.h"
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
#include "domain/ast/stmt.h"
#include "domain/ast/subscript.h"
#include "domain/ast/tuple_expr.h"
#include "domain/ast/while.h"
#include "domain/diagnostics/diagnostic_sink.h"
#include "expression_typer.h"
#include "narrowing_map.h"
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
// list -- a Return is a hit; an If counts when orelse() is NON-EMPTY and BOTH
// branches always return, OR when its condition FOLDS (see
// literal_guard_verdict) and the arm the fold leaves LIVE always returns, so a
// folded-true `if` with an empty else still counts; a While counts only when
// its condition FOLDS TRUE AND its body has no reachable break (see
// contains_reachable_break -- a break belonging to a nested For/While's own
// BODY does not count, since it can never escape THIS loop, but one in that
// nested loop's ORELSE does, since a loop's else runs outside its own break
// scope); a For, or a While whose condition does not fold true, is always
// assumed skippable (false). NOTE the While condition was for a long time
// "the literal `True`, a Constant whose token type is BOOL_TRUE" -- that was
// narrower than mypy, which folds `while 1:` identically; see the second
// bullet below.
//
// This is a syntactic approximation of mypy's real reachability analysis, and
// it does NOT err in only one direction. FIVE failure modes have been
// measured against mypy 1.18.1 so far, and as of 2026-09-17 ALL FIVE ARE
// FIXED -- the first before this file's return-path work began, the third and
// fourth by that work (the fourth being a defect in the very fix for the
// third, caught by adversarial review of that same commit the same day), the
// FIFTH by `a8cae40` (binder narrowing decides break reachability), and the
// SECOND by the literal-folding change that followed it. Do NOT read "all
// five fixed" as "this approximation is now complete": it means only that the
// five measured failure modes are closed. Two SUCCESSORS to the second
// bullet's gap are open and recorded on its own bullet below, and CLAUDE.md
// carries the live list.
//
// A WORD ON THIS TALLY, because it has now been wrong twice. A PRIOR version
// said "the third and fourth are now fixed, the first two remain", which was
// wrong the moment it was written: the first was never open (it is a
// historical entry, not a live gap), so lumping it with the still-open second
// overcounted by one. A LATER version said "exactly two failure modes remain
// open: the second bullet's constant-folding gap, and the fifth bullet's
// narrowing gap" and went STALE when `a8cae40` closed the fifth -- that
// commit touched this very file (+59 lines) but only added new per-function
// comments, leaving this block asserting an open gap it had itself just
// closed. State the fixed/open split the measurements actually show, and when
// you close one of these, update THIS paragraph in the same commit:
//   - FIXED (already, before this task -- a historical entry kept for the
//     record, not a live gap): mypy reports "missing return statement"; we
//     did not: `while True: / for x in xs: pass / else: break` with no
//     return after it. A loop's else runs outside that loop's own break
//     scope, so mypy sees the `break` escape the `while True` and demands a
//     return. This checker once did not look inside a nested loop's orelse
//     at all, so it stayed silent. Re-verified 2026-09-12 against the
//     current binary: cythonpp and mypy now AGREE (both report "missing
//     return statement" on this exact shape), confirming the fix already
//     held and was not itself touched by this task.
//   - A SECOND failure mode, previously open since 2026-09-12 and NOW FIXED
//     (literal_guard_verdict, 2026-09-17): we reported "missing return
//     statement"; mypy did not, for `while True: / if False: / break` with no
//     return after the loop. mypy prunes the `if False:` block as unreachable
//     and never counts that break, so it judges the loop non-terminating and
//     accepts the function, while contains_reachable_break found the `break`
//     textually regardless of the guard around it. The discriminating shape
//     is the one with NO trailing statement after the loop -- a trailing
//     `return 1` would be clean either way, regardless of whether the break
//     folds, so it proves nothing.
//
//     THE PRUNE SET RECORDED HERE WAS INCOMPLETE, and the correction is the
//     part worth carrying forward. This bullet used to state it as
//     `{False, 0, None}` excluding `""`. Re-measured 2026-09-17 across three
//     independent templates: those four claims all hold, but the set also
//     contains the EMPTY TUPLE `()`, and it does NOT contain `0.0` -- so it is
//     neither "numeric zero" nor "any empty container". An int literal folds
//     by VALUE in ANY BASE (`0x0`/`0b0`/`0o0`/`0_0`/`00` all fold false,
//     `0x1`/`1_0` fold true), which is mypy's own `is_true_literal` /
//     `is_false_literal` at `checker.py:8255`. The warning this bullet gave
//     -- that mirroring `is_literal_true` into an `is_literal_false` would
//     SILENCE A REAL ERROR for the `""` case -- was correct and is respected:
//     `""` is not folded. See literal_guard_verdict's own comment in the .cpp
//     for the full measured set and for the two mypy-UNSOUND forms
//     (`NotImplemented`, `not TYPE_CHECKING`) a future widening must refuse.
//
//     `is_literal_true`, which this bullet used to name, NO LONGER EXISTS --
//     it was deleted rather than extended, because it had already DRIFTED:
//     it answered false for `while 1:`, which mypy treats as non-terminating
//     exactly as it does `while True:`. Two rival notions of "literally true"
//     is the condition that produced that drift.
//
//     TWO SUCCESSORS ARE OPEN, and neither is this bullet's gap re-opened.
//     (a) `not`/`and`/`or` over a folded operand are NOT folded, so
//     `while not False:` and `if not True: break` remain false positives --
//     deliberately, because mypy's behaviour there is inconsistent with its
//     own atom rules (`"" or 1` folds TRUE though `""` alone does not fold)
//     and `and` is one-sided where `or` is two-sided. (b) A statically-dead
//     BRANCH's own contents are still type-checked, so `if False: x: int =
//     "s"` is a false "incompatible types in assignment"; that needs a
//     dead-BLOCK model (skip walking the branch), which is strictly more than
//     a folded always-leaves answer.
//   - A THIRD failure mode, previously open and NOW FIXED (loop_else_always_
//     returns, added 2026-09-12): `for i in range(3): / total = total + i /
//     else: / return total` as the whole body of a `-> int` function used to
//     draw `TypeError: missing return statement` from this checker while
//     `mypy --strict` said `Success: no issues found in 1 source file`, and
//     CPython ran it (identically for the `while cond ... else: return`
//     form) -- a real union-rule violation, not merely an imprecision, and
//     the one of these three that needed NO dead code at all to trigger. A
//     loop whose body has no reachable `break` ALWAYS runs its `else`, so
//     that `else`'s return is guaranteed -- always_returns' For/While arms
//     now check exactly that via loop_else_always_returns (see its own
//     comment for the measurement), reusing contains_reachable_break's
//     existing body-vs-orelse descent rule rather than reimplementing it.
//     contains_reachable_break itself also grew a reachability check of its
//     own in the same round: a `break` sitting after a statement that always
//     leaves (e.g. `return 1` / `break` in the same suite) is dead code mypy
//     prunes before ever asking about it, so the scan now stops at the first
//     statement that always leaves, checked AFTER the Break/If/For/While
//     arms above rather than before them -- checking it first would return
//     early on a bare `break` itself (in_loop makes a Break "always leave"
//     too) and on an If whose arms both leave, silently missing a real,
//     reachable break in either case.
//   - A FOURTH failure mode, found by adversarial review of the very commit
//     that fixed the third, THE SAME DAY, and now also FIXED: the reachable-
//     break stop the third bullet just described was placed after the
//     Break/If/For/While arms textually, but each of those arms still ended
//     in an unconditional `continue` -- so control never actually REACHED
//     the stop for any compound statement, only for a bare Return/Continue/
//     leaf. Four shapes were measured mypy-`Success`, running correctly
//     under CPython, and still drawing a false "missing return statement"
//     here: an `if`/`else` where BOTH arms return, a nested `while True:
//     pass` with no break of its own, a nested `for ... else: return`, and
//     an `if c: return 1 else: continue` -- each followed by a `break` that
//     can now never run, exactly the "mypy prunes a dead break, a textual
//     scan does not" defect the third bullet's fix exists to close, just for
//     a compound statement instead of a leaf one. Fixed by restructuring the
//     Break/If/For/While arms into one if/else-if chain so every statement
//     kind, compound or not, falls through to the SAME trailing
//     statement_always_leaves check after its own break-search has already
//     had its chance -- not by adding a second, duplicated check inside each
//     arm. The same round also threaded contains_reachable_break's
//     `in_function` as a REAL parameter instead of a hardcoded `true`: that
//     stop is reached from statement_always_leaves's While/For arms, which
//     check_suite calls at MODULE and CLASS scope too, where a `return` is
//     not legal Python at all, and hardcoding `true` there once silently
//     suppressed a real diagnostic after `while True: return / break` at
//     module scope -- see contains_reachable_break's own comment for both
//     measurements.
//   - A FIFTH failure mode, found by later adversarial review, previously
//     open since 2026-09-12 and NOW FIXED by `a8cae40` (loop_narrows_truthy
//     plus narrowing_guard_verdict, 2026-09-16) --
//     and a DISTINCT gap from the second bullet's constant-folding one --
//     do not fold the two together or describe this as an instance of that
//     one. Measured 2026-09-12: `def f(c: bool) -> int: / while c: / if c: /
//     return 1 / break / else: / return 3` (called as `f(True)` then
//     `f(False)`) draws a false "missing return statement" here, while
//     `mypy --strict` is `Success: no issues found` and CPython prints `1`
//     then `3` at exit 0 -- both oracles accept AND run it, needing no dead
//     code to trigger. The mechanism is mypy's BINDER NARROWING, not
//     constant folding: `while c:` narrows `c` to truthy for the duration of
//     the loop body, so `if c:` inside that body is always true, so the
//     `break` after it is unreachable and the loop's `else` is guaranteed --
//     none of which `contains_reachable_break` can see, since it is purely
//     syntactic and has no notion of a condition variable's narrowed state.
//     See CLAUDE.md's "Semantic analysis traps worth knowing" for the full
//     write-up (a second, independently re-measured variant beyond the one
//     above; a reported count of five was NOT independently confirmed at
//     that count, see CLAUDE.md for the honest tally) and why it is
//     parked for four days behind an explicit precondition -- measure mypy's
//     narrowing per condition shape FIRST -- and that precondition is what
//     made the closure safe: the sweeps found that the DECLARED TYPE decides
//     whether narrowing is usable at all (seven measured non-bool types where
//     mypy REPORTS) and that guard POLARITY decides which arm dies. Both
//     hazards are the same over-/under-aggressive one the second bullet
//     records for constant folding.
//
//     WORTH KNOWING, because the two fixes look alike and are gated on
//     OPPOSITE conditions: narrowing requires the loop condition's declared
//     type to be exactly `bool`, while literal folding is INDEPENDENT of it
//     (measured 2026-09-17: the folded shape is mypy-clean with the condition
//     declared `bool`, `int`, `str`, `float`, `list[int]` and `object`). So
//     folding must NOT be routed through loop_narrows_truthy's bool gate, and
//     must still answer when no narrowed name exists -- that null case is
//     exactly why the `for` sibling of the fifth bullet's shape stayed broken
//     after `a8cae40` fixed the `while` one. See guard_verdict in the .cpp.
// Building real reachability analysis was out of scope for the task that
// wrote this comment, and the syntactic rule catches the overwhelmingly
// common shapes correctly. All five measured failure modes are now closed;
// the second bullet lists the two successors that remain, and CLAUDE.md
// carries the live open-defect list.
//
// Checked once per FunctionDef, at the very end of its body walk, ONLY when
// the function has a return annotation that is neither None nor Unknown --
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
// Attributes come from THREE places, all closing the attribute set at
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
//   - A class-body plain Assign to a bare Name (`class D: x = 5` did not
//     declare a member AT ALL at one point) --
//     handled in assign_to's own Name-target branch, exactly parallel to the
//     AnnAssign case, gated on the SAME is_new_definition this checker
//     already computes for the ordinary bare-empty-container check, so the
//     member's type is the FIRST assignment's inferred type, matching every
//     other "first assignment is sticky" rule in this file.
//   - `self.x = ...` inside ANY method (not just __init__) -- handled in
//     assign_attribute, checked BEFORE the ordinary read path so a brand-new
//     attribute is not a false attr-defined miss. The ANNOTATED form,
//     `self.x: T = ...` (and the value-less `self.x: T`), declares the
//     member the same way from visit(AnnAssign)'s Attribute-target branch,
//     sharing assign_attribute's guard and line disambiguation verbatim; the
//     annotation is the declared type there, where the plain form infers one
//     from the value -- EXCEPT when this class already declares that
//     attribute itself, in which case mypy ignores the annotation outright
//     and the first declaration stays. See that branch for the two measured
//     rules (same class vs. inherited) and why conflating them costs a false
//     TypeError either way round.
//
// ALL THREE of the above were once purely single-pass -- declared only when
// TypeChecker's own visitation actually reached the declaring statement, in
// body order. That
// made a method appearing ABOVE the one that first assigns (or the
// class-body statement that first annotates) an attribute it reads via
// self -- e.g. `def a(self): self.b()` calling a method `b` defined BELOW
// `a`, one of the single most common Python shapes there is -- a false
// attr-defined TypeError. pre_collect_class_body now runs a REAL pre-pass
// over the WHOLE class body (methods' own bodies included, recursively
// through control flow) BEFORE any of it is walked for real, declaring every
// method signature and placeholder-declaring every attribute name up front
// -- see that function's own comment for the full mechanism, including how
// assign_attribute (and assign_to's class-body member declare) recognise
// "this is my own placeholder, fill in the real type" without mistaking it
// for a genuine second, conflicting assignment.
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

// One enclosing function's worth of scope-limited class aliases: every bare
// class name TypeChecker installed into ClassTable
// while walking that function's body, each paired with whatever that same
// bare name resolved to in ClassTable's scoped-alias map BEFORE the install
// (nullopt when nothing did). Torn down in REVERSE order, restoring rather
// than deleting, so an inner function's own same-named local class shadows
// the outer one's for exactly its own body and no longer.
using LocalClassAliasFrame = std::vector<std::pair<std::string, std::optional<std::string>>>;

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

    // The statement lists a MODULE-level or CLASS-level pre-pass must look
    // inside, beyond the list it was handed. Python introduces no scope for
    // an `if`/`while`/`for` block, so a `class` or `def` written there binds
    // its name in the ENCLOSING scope exactly as a flat one does -- verified
    // against mypy 1.18.1, `if FLAG: class Bag: ...` puts "Bag" in module
    // scope and every later use of it is clean, with no "possibly undefined"
    // complaint.
    //
    // THE BOUNDARY: this walk reaches a ClassDef/FunctionDef that SITS inside
    // an `if`/`while`/`for` (same enclosing scope, so its name really does
    // bind here), but never descends INTO a ClassDef's or FunctionDef's own
    // body -- a class declared in a `def` is function-local, owned by
    // visit(ClassDef)'s isolated-name and scoped-alias mechanism, and
    // declaring it from here would leak it to the whole module. A class
    // body's own nested classes are reached by declare_class_recursive
    // instead, which calls back into this function per class body.
    //
    // collect_self_attribute_placeholders no longer shares this boundary: it
    // still never descends into a nested ClassDef (that body's own first
    // parameter belongs to a DIFFERENT class), but it DOES descend into a
    // nested FunctionDef, conditionally -- unless that def rebinds the
    // receiver name as one of its own parameters. mypy
    // attributes a `self.x = ...` store through a capturing closure to the
    // enclosing method's own binding at any depth, so a scan that stopped at
    // every FunctionDef the way this walk does left the attribute undeclared
    // until Phase 3's real walk reached it -- i.e. AFTER a reader sitting
    // above the closure. See collect_self_attribute_placeholders' own
    // comment for the exact rule.
    //
    // `visitor` is invoked for every statement in `body` and in every nested
    // control-flow block, in source order, INCLUDING the If/While/For
    // statements themselves. Its second argument is `directly_in_body` --
    // true only for a statement of the list the OUTERMOST caller passed, and
    // false for anything found inside a control-flow block. Callers that do
    // not care take an unnamed parameter; scan_top_level_names does care, and
    // its comment says why.
    static void for_each_flat_statement(
        const std::vector<ast::StmtPtr>& body, bool directly_in_body,
        const std::function<void(const ast::Stmt&, bool)>& visitor);

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
    //
    // RECURSED THROUGH CONTROL FLOW (see for_each_flat_statement) -- BUT ONLY
    // A CLASS ON EITHER SIDE COLLIDES WITH A CONDITIONAL DEFINITION. Measured
    // against mypy 1.18.1, all seven arrangements:
    //   flat def    + flat def           -> `Name "f" already defined` [no-redef]
    //   flat def    + def inside an if   -> Success
    //   def in if   + def in same if     -> Success
    //   def in if   + flat def           -> `Name "f" already defined` [no-redef]
    //   flat class  + class inside an if -> `Name "Bag" already defined`
    //   class in if + class in else      -> `Name "Bag" already defined`
    //   flat def    + class inside an if -> `Name "Bag" already defined`
    // mypy allows a CONDITIONAL FUNCTION redefinition, but ONLY ONE order --
    // "def in if + flat def" DOES collide, and this rule stays silent there
    // too, since only the first occurrence's flatness is recorded; a
    // deliberately over-applied, missed-error-not-false-error allowance (see
    // scan_top_level_names' own comment). mypy also allows no conditional
    // class redefinition at all. So the collision fires when either
    // definition is a class, or when both sit flat in the module body -- see
    // TopLevelDefinition and the function's own body for why the kind
    // and the flatness are both recorded rather than just the line.
    void scan_top_level_names(const ast::Module& module);

    // Phase 1: declare every top-level ClassDef's name and bases into
    // ClassTable (no annotation resolution yet -- a base may name a class
    // declared later in the same module), RECURSING into each class's own
    // body to declare a NESTED ClassDef too, under its qualified name (see
    // declare_class_recursive) -- so `x: Outer.Inner` resolves in Phase 2,
    // which runs before Phase 3 (the ordinary walk) ever reaches Inner's own
    // ClassDef node. A top-level ClassDef scan_top_level_names already
    // reported as a collided redefinition is SKIPPED here: declaring it
    // anyway silently overwrote the winning
    // same-named class's ClassTable entry. Each class's bases are RESOLVED as
    // that class is declared, so a base naming a class not declared yet is
    // reported right there by AnnotationResolver. THEN -- once every class is
    // declared -- validate_class_bases walks the resolved bases for the one
    // shape the resolver cannot judge on its own, a `tuple` base.
    void collect_classes(const ast::Module& module);

    // One class ClassTable holds an entry for, paired with the exact
    // qualified name that entry is keyed under -- "Outer.Inner" for a nested
    // class, the bare name at the top level. Recorded by
    // declare_class_recursive while every class is being declared, and
    // consumed twice afterwards: by validate_class_bases, and by the
    // member-collection phase, which needs the SAME qualified name
    // pre_collect_class_body would later have been called with.
    //
    // `base_types` is the SAME resolution that fed ClassTable::declare for
    // this class, recorded again here rather than re-derived, so
    // validate_class_bases can check a resolved base's Type (the `tuple` base
    // deferral) without invoking AnnotationResolver a
    // second time -- which would double-report a bad base. One entry per
    // base expression, in the same order, by construction: both are built
    // from a single `base_types(...)` call.
    struct ClassDeclaration {
        const ast::ClassDef* node = nullptr;
        std::string qualified_name;
        std::vector<Type> base_types;
    };

    // The recursive half of collect_classes: declares `class_def` under
    // `qualified_prefix + "." + class_def.name()` (or just its own name, at
    // the top level, where `qualified_prefix` is empty), appends it to
    // `all_classes` for the base-validation loop collect_classes runs once
    // every class -- at every nesting depth -- is declared, then recurses
    // into `class_def`'s own body for a nested ClassDef, passing ITS OWN
    // qualified name down as the next prefix.
    void declare_class_recursive(const ast::ClassDef& class_def, const std::string& qualified_prefix,
                                 std::vector<ClassDeclaration>& all_classes);

    // The base-validation half of collect_classes, extracted
    // so declare_isolated_class below can reuse it for a class ClassTable
    // never saw during Phase 1. ONE rule: a base whose resolved Type is a
    // `tuple` draws a NotImplementedError, because mypy and CPython disagree
    // about what a tuple subclass IS (see the body for the measurement) and
    // this compiler has to emit code that runs.
    //
    // Everything else a base can get wrong is reported EARLIER, by
    // AnnotationResolver, at the moment base_types resolved it, and reaches
    // this loop as Unknown. That includes EXECUTION ORDER, which used to be
    // an explicit rule here and is now emergent: bases are resolved during
    // the declaration walk, in source order, so `class Child(Parent):`
    // written above `class Parent:` cannot resolve and is reported at the
    // base expression -- which is what the union rule demands, since CPython
    // raises NameError there while mypy is order-insensitive. The explicit
    // rule was deleted once the resolver subsumed it: its only remaining
    // output was a false NameError on a builtin shadowing both oracles
    // accept.
    //
    // Runs only once every declaration in `all_classes` exists. It needs
    // nothing from ScopeStack, so it does not have to wait for the
    // name-binding phases.
    void validate_class_bases(const std::vector<ClassDeclaration>& all_classes);

    // Declares `node` (and, recursively, every
    // ClassDef nested in its own body) into ClassTable under `qualified_name`
    // -- returned back to the caller unchanged, for use exactly like an
    // ordinarily-declared one for the REST of that class's handling
    // (ClassContextGuard, pre_collect_class_body, self's binding). Both
    // callers (the function-local case, and the losing
    // top-level collision -- both listed below) ALWAYS pass a freshly
    // synthesised name that
    // embeds `#`, a character no Python identifier can ever contain, so it
    // can never collide with any legitimately dotted "Outer.Inner" name
    // collect_classes produced, or with any other class's bare name.
    //
    // The function-local case used to pass the class's
    // plain BARE name whenever that name was not already live in ClassTable
    // -- kept, back then, specifically so a function-local `Local()` call
    // stayed resolvable as a constructor from inside its own defining
    // function (classes_.is_class(identifier), the constructor-call
    // dispatch's own lookup in expression_typer_calls.cpp, has no scope
    // awareness at all). That bare-name declare() has no collision detection
    // of its own, though: a SECOND, later-declared local class of the
    // identical name (a different function's own same-named local class)
    // silently overwrote the first one's entry, so a call from inside the
    // second function resolved to the FIRST function's class and a member
    // access on the result was a FALSE attr-defined TypeError -- and the
    // bare name stayed a LIVE ClassTable entry for the rest of the module's
    // Phase-3 walk, turning a correct NameError for a later, unrelated
    // module-level use of the same bare name into a silent false
    // acceptance. Always isolating under the synthetic name closes both
    // holes.
    //
    // An earlier version accepted, as a supposed cost of that
    // isolation, that `Local()` could no longer be resolved as a constructor
    // call at all -- even from inside its own defining function. That was
    // not a missed error but a FALSE one (`NameError: name 'Local' is not
    // defined` on a program mypy 1.18.1 accepts), on EVERY function-local
    // class construction. There is now no such cost: the isolated key stays,
    // and visit(ClassDef) additionally installs a SCOPE-LIMITED alias from
    // the bare source-level name to it (ClassTable::declare_scoped_alias),
    // removed by LocalClassAliasGuard when the enclosing function's body
    // walk ends -- so the bare name resolves inside that function and
    // nowhere else. See visit(ClassDef)'s own comment.
    //
    // Two, unrelated situations both need this because neither one was ever
    // reached by collect_classes' Phase-1 walk, which only recurses into
    // MODULE-level and CLASS-level bodies:
    //   - a ClassDef lexically inside a `def` (or any other
    //     non-module, non-class scope);
    //   - a top-level ClassDef scan_top_level_names already
    //     reported as a LOSING same-name collision. Phase 1 already skips
    //     the loser's own declare() call, but Phase
    //     3 still walks its body like any other statement (matching how a
    //     colliding top-level FunctionDef's body is still checked) -- without
    //     this, that walk would write the loser's members/methods (and,
    //     worse, an __init__) onto the WINNER's entry under the identical
    //     bare qualified name: a missed error (the loser's extra members
    //     silently merge onto the winner), a silent wrong type
    //     (declare_member overwrites with no comparison), and false
    //     diagnostics (the loser's __init__ overwrites the winner's,
    //     producing wrong arity errors at every legitimate `C(...)` call).
    //     Isolating the loser under its own unreachable name means its body
    //     is still checked for diagnostics (unchanged), but into an entry
    //     nothing else ever queries -- nothing refers to a losing top-level
    //     class by name, since scan_top_level_names already reported the
    //     redefinition and no downstream lookup resolves "C" to it.
    std::string declare_isolated_class(const ast::ClassDef& node, const std::string& qualified_name);

    // The exact analogue of
    // collect_signatures at module level, but for ONE class body, run from
    // visit(ClassDef) right after ClassContextGuard is constructed and
    // BEFORE any of the class's own body statements are walked --
    //   - every direct AnnAssign's annotation is resolved (and, if a bad
    //     annotation, reported) FIRST, in its own dedicated sub-pass over the
    //     WHOLE body, cached in
    //     class_body_annotation_types_ so visit(AnnAssign)'s own later walk
    //     of that SAME node reuses it rather than invoking AnnotationResolver
    //     (and so double-reporting a bad annotation) a second time, and
    //     declared into ClassTable via declare_member UNLESS a member or
    //     method under that name already exists (the same
    //     has_value() guard -- reachable now only for a second class-body
    //     AnnAssign of the same name, since this sub-pass runs before any
    //     self.x placeholder ever could exist);
    //   - every direct FunctionDef (i.e. every method) gets its signature
    //     resolved and declared into ClassTable via declare_method
    //     immediately (see resolve_method_signature), and cached in
    //     class_method_signatures_ so visit(FunctionDef)'s own later walk of
    //     that SAME node reuses it rather than invoking AnnotationResolver
    //     (and so double-reporting a bad annotation) a second time -- mirrors
    //     top_level_signatures_'s own contract exactly;
    //   - every direct plain Assign to a bare Name is placeholder-declared
    //     (Type::unknown(), at ITS OWN line) the same has_value()-guarded
    //     way, so `class D: x = 5` registers "x" as an attribute at all --
    //     the REAL inferred type is filled in later, when
    //     Phase 3's own visit(Assign) actually reaches this exact statement
    //     (see assign_to's own is_new_definition-gated declare_member call,
    //     itself now guarded against overwriting a genuine earlier
    //     declaration);
    //   - every method's OWN body is, in turn, scanned (recursively through
    //     If/While/For, matching pre_bind_function_body's own scope
    //     boundary: NOT into a nested def) for a `self.x = ...` assignment,
    //     via collect_self_attribute_placeholders -- so the attribute
    //     exists (at the line of its own FIRST such assignment; as an
    //     Unknown placeholder for the plain form, as the resolved
    //     annotation for `self.x: T = ...`) before ANY method's body --
    //     including one
    //     occurring EARLIER in the class body -- is actually walked. This is
    //     the whole point of the sub-pass: `def a(self): self.b()` reading a
    //     method `b` defined below `a`, or reading an attribute a later
    //     method first assigns, no longer depends on visitation order.
    //
    // The AnnAssign sub-pass runs BEFORE the
    // method/plain-Assign sub-pass below, over the WHOLE body, rather than
    // interleaved with it in textual order -- verified
    // against mypy 1.18.1, a class-body annotation is the DECLARED type of
    // that attribute for the WHOLE class body regardless of where it
    // appears textually, so it must win over a self.x placeholder
    // REGARDLESS of which one is textually first (an interleaved,
    // textual-order pass got this backwards whenever the annotation
    // appeared BELOW the self.x assignment it conflicts with -- see
    // AClassBodyAnnotationConflictingWithAnEarlierSelfAssignmentIsReported).
    // This reordering is sound specifically because an annotation's type
    // comes from the annotation EXPRESSION alone (order-independent), unlike
    // a plain Assign's inferred type, which stays interleaved in textual
    // order with methods below because it genuinely depends on a
    // (possibly order-sensitive) VALUE expression Phase 3 alone can safely
    // resolve.
    //
    // Both sub-passes are recursed through control flow (see
    // for_each_flat_statement): Python introduces no scope for an `if` inside
    // a class body, so a method or an annotation written there is a member of
    // the class exactly as a flat one is. visit(AnnAssign)'s own class-body
    // arm already behaved this way (it gates on the CURRENT scope being
    // Class, which an `if` does not change); this makes the pre-pass agree
    // with it instead of missing the nested case.
    void pre_collect_class_body(const ast::ClassDef& node, const std::string& qualified_name);

    // The method-signature half of pre_collect_class_body's per-FunctionDef
    // work, factored out because visit(FunctionDef)'s own (uncached) branch
    // needs the identical self-parameter rule (index 0, unannotated, exempt
    // -- bound to Class(qualified_name) instead of Unknown, the ORIGINAL
    // "self upgrade" from Task 19) and this is the one place both call sites
    // can share it without drifting apart. Deliberately does NOT compute
    // any_param_missing/any_param_annotated or report anything about the
    // signature's OWN completeness -- that diagnostic still fires exactly
    // once, later, when Phase 3's real visit(FunctionDef) reaches this same
    // node (reusing this resolution from class_method_signatures_ rather
    // than re-deriving it).
    Type resolve_method_signature(const ast::FunctionDef& method, const std::string& qualified_name);

    // The recursive walk
    // pre_collect_class_body runs over EVERY method's own body (If/While/For
    // recursed into, matching pre_bind_function_body's scope boundary)
    // looking for `receiver_name.x = ...` -- a plain Assign OR an
    // AnnAssign (`receiver_name.x: T = ...`, and the value-less
    // `receiver_name.x: T` too) whose target is an Attribute on a bare Name
    // spelled `receiver_name`, the method's own first parameter (see the call
    // site: it is always that method's `params().front().name`, whatever it
    // is spelled). The ANNOTATED form IS resolved here, and the resolution is
    // CACHED in self_annotation_types_ (see there) so visit(AnnAssign) reuses
    // it instead of resolving a second time; the plain form has no annotation
    // to resolve and stays Unknown. The FIRST such
    // occurrence for a given attribute name (in this same top-to-bottom scan
    // order) that names neither an existing member NOR an existing method is
    // placeholder-declared at ITS OWN line. This is what
    // lets assign_attribute's real, later pass over that EXACT statement
    // recognise "this is my own placeholder, fill in the real type" (line
    // equality, exactly like is_unfilled_placeholder's ScopeStack analogue)
    // rather than mistaking it for either a genuinely new declaration (there
    // is no "genuinely new" left once every attribute is placeholder-declared
    // up front) or a second, real conflicting assignment.
    //
    // A nested def IS recursed into -- unless it REBINDS `receiver_name` as
    // one of its own parameters, in which case that
    // parameter is a different binding and the scan must not descend. mypy
    // attributes a store through a captured receiver to the enclosing
    // method's own binding at ANY closure depth (measured: a reader ABOVE
    // such a closure is mypy `Success` and CPython runs it), so this scan's
    // refusal to descend used to leave the attribute undeclared until Phase
    // 3's real walk reached it -- i.e. AFTER a reader sitting above. A nested
    // ClassDef is still never recursed into: its methods' own first
    // parameter belongs to the INNER class, so a store there declares onto
    // that class, not this one.
    void collect_self_attribute_placeholders(const std::string& qualified_name,
                                             const std::string& receiver_name,
                                             const std::vector<ast::StmtPtr>& body);

    // One statement's worth of the scan above, shared by its plain-Assign and
    // its AnnAssign arm so the two forms cannot drift into recognising
    // different sets of targets. A no-op unless `target` is an Attribute on a
    // bare Name spelled exactly `receiver_name` whose attribute name has
    // neither a member nor a method already.
    //
    // `annotated_statement` is the AnnAssign for the annotated form and
    // nullptr for the plain one -- the node itself rather than a bool,
    // because the annotated form needs both halves of it: the annotation to
    // resolve, and the node's own address to key the resolution cache under.
    //
    // WHAT TYPE gets declared follows from that. The plain form has no
    // declared type to know yet, so it installs Type::unknown() and leaves
    // the real one to the walk. The annotated form installs the RESOLVED
    // ANNOTATION. Installing Unknown for it instead is not a harmless
    // approximation: Unknown is absorbing, so every earlier `self.x = ...`
    // in the same class stops being checked against the type the attribute
    // actually has, and `class Base: v: int` / `class Child(Base)` with a
    // method assigning `self.v = "s"` ABOVE a method declaring
    // `self.v: int = 1` silently loses an incompatible-assignment error mypy
    // reports. Resolving eagerly is safe for the same reason the class-body
    // annotation sub-pass resolves eagerly: an annotation's type comes from
    // the annotation expression alone, and every class is already declared.
    // Resolving it TWICE is the hazard, and the cache is what prevents it.
    //
    // Note this does NOT resolve the receiver through ScopeStack the way
    // self_attribute_receiver_type does -- it CANNOT, since no scope is
    // pushed during a pre-pass. Its scope discipline is structural instead:
    // collect_self_attribute_placeholders recurses into a nested def only
    // when that def does not rebind `receiver_name`, and never into a nested
    // class, so the only receiver this scan can see is either the enclosing
    // method's own first parameter or a closure over it that does not shadow
    // it. The real walk re-checks the binding properly (via `method_self`)
    // before filling any placeholder in, so a shadowing parameter still
    // declares nothing real regardless of what this structural scan alone
    // could tell.
    //
    // WHICH GUARD, and it depends on whether the form is annotated -- the
    // two forms are not one rule. Measured against mypy 1.18.1:
    //
    //  - ANNOTATED (`self.v: int = 1`) IS a per-class declaration that
    //    narrows an inherited attribute, and it holds for the WHOLE class
    //    including a reader method ABOVE it (`class Base` declaring
    //    `self.v: object`, `class Child(Base)` whose `use` reads `self.v + 1`
    //    above its own `self.v: int = 1`, is `Success`). So this form must be
    //    placeholder-declared even when a BASE already declares the name --
    //    hence own_member_type, the direct-entry lookup, which is the only
    //    query that can tell "this class already declares it" from "a base
    //    does".
    //  - PLAIN (`self.v = 0`) is NOT. `class Base: v: object` with a Child
    //    that both reads `self.v + 1` and assigns `self.v = 0` still reports
    //    `Unsupported operand types for + ("object" and "int")`, and
    //    `class Base: v: int` with a Child doing `self.v = "s"` still
    //    reports `Incompatible types in assignment`. So this form must keep
    //    the chain-walking member_type guard: declaring a placeholder on the
    //    subclass would make the assignment its own first declaration and
    //    silence that second, CORRECT error for nothing in return.
    //
    // The METHOD half of the guard stays chain-walking for both forms: a
    // base's method name is genuinely taken, and nothing measured here says
    // otherwise.
    void declare_self_attribute_placeholder(const std::string& qualified_name,
                                            const std::string& receiver_name,
                                            const ast::Expr& target, int line,
                                            const ast::AnnAssign* annotated_statement);

    // Phase 2: resolve every top-level FunctionDef signature and every
    // module-level AnnAssign's annotation, binding each name into ScopeStack
    // with its declaration line -- so a function body (once Task 18 checks
    // one) or a later statement can see a class declared below it (Phase 1
    // already ran) or a name declared below it in source order without a
    // false NameError once Task 18 wires up FunctionDef bodies.
    //
    // RECURSED THROUGH CONTROL FLOW for the FunctionDef arm only (see
    // for_each_flat_statement): Python introduces no scope for an
    // `if`/`while`/`for` block, so a `def` written there binds its name in
    // module scope exactly like a flat one -- the same rule collect_classes
    // already applies to a conditional `class`. Before this recursion, a
    // conditional def's own name was bound by nobody (its own
    // visit(FunctionDef) binds it only when the current scope is Function,
    // which at module level it never is), so every call to one was a false
    // NameError on code mypy accepts and CPython runs.
    //
    // The AnnAssign arm gets this recursion too, unconditionally: a
    // conditional annotated assignment is bound here exactly like a flat one,
    // at its own line -- which is what makes a read above it say "used before
    // definition" instead of "not defined" (matching mypy), and what makes a
    // later same-name definition report against the right statement.
    //
    // A conditional def's FAILED bind is dropped silently only when the
    // binding it collided with also came from a def: measured against mypy
    // 1.18.1, two defs of one name in an if/else, and two in the same block,
    // are both `Success` -- mypy allows a conditional function redefinition,
    // so reporting one here would be a false TypeError on mypy-clean code.
    // The first binding wins; a second, genuinely incompatible conditional
    // def is a missed error, the safe direction, matching the allowance
    // scan_top_level_names already makes for the identical case. A def
    // colliding with a VARIABLE binding (an annotated assignment or not) is a
    // different collision class mypy always reports, conditional or not --
    // measured `Incompatible redefinition` -- so that failed bind still
    // reports below; see the def-vs-def tracking set local to this
    // function's own definition for how the two are told apart.
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
    //
    // Walks `module.body()` DIRECTLY, not recursively into a control-flow
    // block, so an assignment written inside an `if`/`while`/`for` gets no
    // placeholder. The consequence, measured: `print(x)` above
    // `if FLAG: x = 5` reports `name 'x' is not defined` where mypy reports
    // `Name "x" is used before definition  [used-before-def]` and CPython
    // raises `NameError: name 'x' is not defined`. All three reject the
    // program, so that is a wording gap, not a compliance one. Recursing here
    // was tried and reverted: nothing in this pass needed it once
    // validate_class_bases stopped asking ScopeStack about a dotted base's
    // root, and the wider traversal made an ordinary loop read
    // (`for line in [...]:` above a later `if True: line = "z"`) a false
    // NameError on a program BOTH oracles accept (measured: mypy --strict
    // "Success: no issues found in 1 source file"; CPython prints `2 z`).
    //
    // WHICH names get a placeholder is therefore still the top-level walk's
    // answer, and that hazard is untouched. What DOES consult the recursive
    // walk (2026-09-16) is the LINE each placeholder carries -- a strictly
    // separate question, and the old answer was wrong twice over: a name
    // first bound inside an `if`/`while`/`for` and assigned again at top
    // level got its placeholder stamped with the LATER top-level line, so
    // (a) the earlier, genuinely-first assignment could not fill its own
    // placeholder (is_unfilled_placeholder compares lines) and the later
    // assignment won the declared type, losing mypy's `Incompatible types in
    // assignment` entirely, and (b) a read sitting BETWEEN the two reported a
    // false `used before definition` against that later line. See
    // pre_bind_assignment_targets' own definition for the measurements, and
    // for why a nested def/class name is excluded from the map and
    // loop_start_line deliberately is not carried.
    void pre_bind_assignment_targets(const ast::Module& module);
    void pre_bind_target(const ast::Expr& target, int line,
                         const std::map<std::string, int>& first_binding_line);

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

    // Each base expression as a Type, for ClassTable::declare, resolved
    // through AnnotationResolver -- the SAME resolver an annotation goes
    // through, so `list[int]` becomes list[int] here exactly as it does in
    // `x: list[int]`, with one implementation and one set of diagnostics.
    //
    // This used to keep only a bare Name and drop every other base shape on
    // the floor, which is the root cause of `class IntList(list[int])` being
    // unusable: with the base discarded, the subclass reached no builtin at
    // all, so `x: list[int] = IntList()` was a false TypeError and both
    // `IntList()[0]` and iterating one were deferrals.
    //
    // A base that fails to resolve becomes Unknown, which the chain walk
    // simply skips (see ClassTable::base_key) -- one root cause, one
    // diagnostic, reported by the resolver itself.
    //
    // ONE EXCEPTION: a base that is ITSELF, at the top level, an ast::
    // Attribute -- a plain dotted base like `Outer.Inner` or `mod.Thing` --
    // is skipped BEFORE it reaches the resolver, pushing Unknown with no
    // diagnostic at all. AnnotationResolver's own attribute handling would
    // report a NameError for cases this compiler deliberately leaves alone;
    // the body's own comment records that history and the two measured
    // missed errors it accepts.
    //
    // The exception is exactly that shape and no wider. A base whose top
    // level is something else goes through the resolver even when a dot
    // appears INSIDE it: `class D(mod.Thing[int]): pass` is a Subscript, and
    // it reports -- measured, `TypeError: not a valid type annotation` at the
    // base, against mypy's `Name "mod" is not defined` plus `Class cannot
    // subclass value of type "Any"` and CPython's `NameError: name 'mod' is
    // not defined`, so both oracles reject it too.
    //
    // Called TWICE, from two different phases, and the difference matters:
    //
    //   - From declare_class_recursive, during the DECLARATION pass, as each
    //     module-or-class-level class is declared -- so a base naming a class
    //     declared LOWER in the module has not been declared yet and the
    //     resolver reports it. That is not a limitation but the intended
    //     behaviour: CPython raises NameError from such a `class` statement,
    //     so the union rule refuses the program. Resolving eagerly is safe
    //     here for the same reason a class-body annotation is: a base
    //     expression names types, and nothing in it depends on an inferred
    //     value.
    //   - From declare_isolated_class, during the ORDINARY walk, for a
    //     function-local or collision-losing class. That later timing is
    //     load-bearing rather than incidental: by then Phase 1 has declared
    //     every module-level class, so `def f(): class Local(Later): ...`
    //     written above `class Later:` resolves and stays clean -- which is
    //     correct, because the function body runs at CALL time, after the
    //     module-level `class Later:` statement has executed.
    std::vector<Type> base_types(const std::vector<ast::ExprPtr>& bases);

    // How many of `params` carry a default value, for Type::callable's
    // `defaulted` argument -- the count a call site needs in order NOT to
    // report "too few arguments" for a call that legitimately omits them.
    // Counted from the END: Python's grammar already guarantees defaulted
    // parameters are trailing, so a plain count is the trailing count, and
    // this is the ONE place the AST's Parameter::default_value is turned
    // into that number, so no signature producer can compute it differently.
    static std::size_t defaulted_param_count(const std::vector<ast::Parameter>& params);

    // The names of `params`, in order, for Binding::param_names -- the ONE
    // place the AST's Parameter::name list becomes that vector, so the two
    // `def`-binding sites cannot record it two different ways.
    static std::vector<std::string> param_names_of(const std::vector<ast::Parameter>& params);

    // mypy's "All conditional function variants must have identical
    // signatures": whether a `def` whose signature is `signature` (parameter
    // names `param_names`) may silently redefine the name `existing` already
    // holds. It answers ONLY the signature half of that rule -- the caller
    // still has to establish that the redefinition is a conditional `def`.
    //
    // A THIRD comparison, deliberately neither of the two that already exist,
    // and the reason each is wrong here is worth stating once:
    //
    //   - `Type::operator==` is exact and UNION-ORDER-SENSITIVE (type.h says
    //     so, and says the asymmetry with is_subtype is intentional). mypy's
    //     identity is order-INsensitive: measured 2026-09-11 with mypy
    //     1.18.1, conditional variants `def g(a: int | str)` and
    //     `def g(a: str | int)` are accepted, and so are `int | None` versus
    //     `None | int`, a three-member rotation, `int | (str | float)` versus
    //     `float | str | int`, and a union nested inside a `list`, `dict` or
    //     `tuple` argument. Using == here reported a TypeError on every one
    //     of those -- a false positive on code both oracles accept.
    //   - `is_equivalent` (mutual is_subtype) is order-insensitive but too
    //     LOOSE in three ways that each turn a real error into silence:
    //     Unknown is absorbing in is_subtype, so an unresolvable annotation
    //     would match anything; the Callable arm deliberately ignores
    //     `defaulted_params`, while mypy treats `def g(a: int = 1)` and
    //     `def g(a: int)` as DIFFERENT signatures (measured: `All conditional
    //     function variants ...`); and a class name is canonicalised through
    //     the base chain.
    //
    // So this is structural identity, exactly as `operator==` computes it,
    // with Union members matched as an unordered set at every depth -- and
    // then parameter names compared on top, which no comparison of two
    // `Type`s could do at all (see Binding::param_names for why the names
    // live on the binding and what "empty" there means).
    //
    // Static because it consults no ClassLookup: two Class types must agree
    // on their `name` exactly. Both sides arrive already canonicalised by
    // AnnotationResolver -- measured, `def g(a: IOError)` against
    // `def g(a: OSError)` is mypy-clean and accepted here -- so routing
    // through ClassLookup would buy nothing and would give this rule a
    // dependency the question does not have.
    static bool has_identical_signature(const Binding& existing, const Type& signature,
                                        const std::vector<std::string>& param_names);

    // Resolves `annotation` and attempts to bind `target` into the CURRENT
    // scope with it (annotated = true). If the name is already bound there,
    // reports the settled redefinition wording and returns
    // {type, redefinition = true} without touching the existing binding --
    // the first declaration wins, matching ScopeStack::bind's own contract.
    AnnotationBinding bind_annotation(const ast::Name& target, const ast::Expr& annotation,
                                      int line);

    // The scope-binding HALF of bind_annotation, factored out so
    // a class-body AnnAssign whose annotation was ALREADY resolved (and
    // reported on) by pre_collect_class_body's own eager pass can still get
    // the ordinary scope-bind/redefinition treatment without invoking
    // AnnotationResolver a second time -- which would double-report a bad
    // annotation. bind_annotation itself is now a thin wrapper: resolve, then
    // delegate here.
    AnnotationBinding bind_resolved_annotation(const ast::Name& target, Type type, int line);

    // Assign, dispatched by target shape.
    void assign_to(const ast::Expr& target, const ast::Expr& value, int line);
    void assign_tuple(const ast::TupleExpr& target, const ast::Expr& value, int line);
    void assign_subscript(const ast::Subscript& target, const ast::Expr& value);

    // A dict SUBSCRIPT STORE is the second mypy PARTIAL CONTAINER resolver
    // form, alongside assign_name's matching non-empty display: `x = {}` /
    // `x["a"] = 1` declares `dict[str, int]`. Called from assign_subscript
    // BEFORE it reads the target's element type, which for a live partial
    // answers Unknown and so checks nothing. A LIST subscript store
    // deliberately resolves NOTHING (control C3) -- see the function's own
    // comment and the kind matrix in resolvable_container_partials.
    bool resolve_dict_partial_from_store(const ast::Subscript& target,
                                        const Type& value_type);

    // Task 19: `self.x = ...` inside a method DECLARES a new instance
    // attribute the first time TypeChecker's own single-pass visitation
    // encounters it for a given name -- checked FIRST, syntactically plus one
    // ScopeStack::resolve (never typed, so this check alone cannot itself
    // report anything): the receiver is a bare Name -- whatever it is spelled,
    // not necessarily "self" -- that currently resolves to
    // Class(current_class_qualified_name_) through its OWN method_self
    // binding (see self_attribute_receiver_type and Binding::method_self).
    // Only once
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

    // A path's DECLARED type -- the ceiling every narrowing layers over, and
    // what a join uses for an edge that never assigned the path. std::nullopt
    // when the path does not resolve at all, which is a path with nothing to
    // narrow rather than an error (the read itself reports, if it is one).
    //
    // The root resolves through ScopeStack, so it is the CURRENT scope's
    // reading of that name -- which is why the function-boundary reset must
    // clear the map rather than merely shadow it: an entry surviving into a
    // scope where its root means something else would be a narrowing of the
    // wrong variable.
    //
    // Each subsequent link needs its receiver to be a Class, resolved through
    // ClassTable's chain-walking member lookup, so an INHERITED attribute has
    // a declared type here exactly as an own one does.
    std::optional<Type> declared_type_of_path(const NarrowedPath& path) const;

    // An annotation RE-declares a path, so anything known about that path
    // (and about anything beneath it) is stale.
    //
    // Whether its value then NARROWS it depends on the target's shape, and
    // the two halves are asymmetric -- verified against mypy 1.18.1. A bare
    // NAME does NOT narrow from its own declaring statement: `x: object = 5`
    // reveals `builtins.object` on the next read, at module scope, function
    // scope and class-body scope alike, so narrowing a plain variable only
    // ever comes from a SEPARATE, later plain reassignment. An ATTRIBUTE
    // target DOES, even a brand-new one: `self.n: object = 5` reveals
    // `builtins.int`. The caller (visit(AnnAssign)) is what branches on
    // that; this function just does what its argument says. See that call
    // site for the full statement of the rule.
    //
    // `narrowed` is std::nullopt for a value-less annotation (`x: int`), for
    // every bare Name target per the asymmetry above, and
    // for one whose value was already reported as incompatible -- in each
    // case the path is killed and left at its declared type, which for the
    // incompatible one is the point: narrowing
    // to a type the annotation forbids is exactly what the declared-type
    // ceiling exists to prevent.
    void redeclare_narrowing(const ast::Expr& target, const std::optional<Type>& narrowed);

    // THE `self.x` GUARD, in ONE place: the Class type `self` is bound to
    // when `target` really is an attribute store on the enclosing class's
    // own instance, and std::nullopt otherwise. Shared -- literally, not by
    // a second copy -- by assign_attribute (plain `self.x = ...`) and
    // visit(AnnAssign)'s Attribute-target branch (`self.x: T = ...`), which
    // must agree on it exactly: any divergence would let one form declare a
    // member the other refuses to, and the resulting attr-defined TypeError
    // would depend on which form was written.
    //
    // Purely syntactic plus one ScopeStack::resolve (the receiver is never
    // itself typed here, so this check alone can never report anything): the
    // receiver must be a bare Name -- resolved by ITS OWN spelling, whatever
    // that is, not the literal "self" (measured 2026-09-12: a method's first
    // parameter named `this` is mypy `Success` and CPython runs it, where
    // keying on the spelling "self" reported twice) -- that resolves to a
    // binding which BOTH has type Class(current_class_qualified_name_) AND is
    // a method's own first parameter (Binding::method_self).
    //
    // BOTH conditions are load-bearing, and the second was added only after
    // the first alone proved insufficient. The type check stops a
    // nested function whose own `self` is bound to a DIFFERENT type --
    // `def inner(self: int) -> None: self.q = 1` inside a Bag method must not
    // declare "q" on Bag, and mypy reports its own attr-defined error there.
    // What it does NOT stop is a nested `def inner(self: Bag)` inside a Bag
    // method: that `self` resolves to exactly Class("Bag"), so a type-only
    // guard passed and `self.q = 1` there declared "q" on Bag, making a later
    // `self.q` read come out clean where mypy reports `"Bag" has no attribute
    // "q"` at both the store and the read. The method_self check is what
    // tells those apart.
    //
    // It is deliberately a fact about the BINDING rather than about the
    // innermost function. Requiring "the immediately enclosing function is
    // itself a method" was tried and MEASURED WRONG in the unsafe direction:
    // mypy attributes a store to the method's self through any number of
    // capturing closures, so a `def inner()` inside a Bag method writing the
    // captured `self.q = 1` is `Success` (as is the same store two closures
    // deep), and that version reported a false TypeError on every one. The
    // flag travels with the name, so resolving outward through a closure
    // still finds the method's own `self`. See Binding::method_self.
    std::optional<Type> self_attribute_receiver_type(const ast::Attribute& target) const;

    // Which of three states a `self.x` store's attribute name is in --
    // pre_collect_class_body placeholder-declares, at ITS OWN line, the
    // first `self.x = ...` (as Unknown) AND the first `self.x: T = ...` (as
    // the resolved T) it finds
    // scanning every method's body up front, so by the time this real,
    // single-pass walk reaches ANY of them ClassTable already has an entry
    // for practically every attribute and a bare "does a member/method exist
    // already" test can no longer tell "brand new" from "this IS my own
    // placeholder, fill it in". The declared LINE is the disambiguator,
    // exactly like is_unfilled_placeholder's ScopeStack analogue.
    //
    // ExistingDeclaration covers BOTH a member declared at a DIFFERENT line
    // (a genuine earlier, real assignment or annotation) and a same-name
    // METHOD -- the two cases every caller handles the same way, by NOT
    // treating the statement as this attribute's own first declaration.
    enum class SelfMemberState { BrandNew, OwnPlaceholder, ExistingDeclaration };
    SelfMemberState self_member_state(const std::string& attribute, int line) const;

    // The one place a Name target is bound or checked, for both a plain
    // Assign and each element of a tuple-unpacking Assign. See
    // pre_bind_assignment_targets for what "my own still-unfilled
    // placeholder" means and why declared_line == line is the signal for it.
    //
    // `order_exempt` defaults to false for
    // every ordinary assignment, but a `for` target's own first bind passes
    // true: like a parameter, it is bound before its body ever runs, so a
    // one-line suite (`for i in range(3): print(i)`) reading it within that
    // same body can never be a genuine use-before-definition, only a false
    // positive from the ordinary `declared_line >= read_line` check. The
    // FRESH-bind branch honours this flag directly; the placeholder-fill
    // branch threads it through too (since 2026-09-13, when
    // pre_bind_function_body started pre-binding a `for` target the same way
    // it already pre-bound an Assign/AnnAssign target, which made that branch
    // reachable for a `for` target for the first time) -- the reassignment
    // branch never builds a new Binding, so there is nothing for it to change
    // there.
    //
    // `partial_container` is the mypy PARTIAL CONTAINER shape this assignment
    // seeds, or nullopt (the overwhelmingly common case). Only the two
    // BINDING branches consume it -- a fresh bind and a placeholder fill,
    // exactly the pair seeds_partial_none already has to agree across -- and
    // the caller passes a value only when assign_to has both established this
    // is the name's FIRST definition and confirmed with the per-scope scan
    // that the partial is resolved before it is read. See
    // Binding::partial_container for why this is a separate rule from
    // partial_none rather than an extension of it.
    void assign_name(const ast::Name& target, const Type& value_type, int line,
                     bool order_exempt = false,
                     const std::optional<Type>& partial_container = std::nullopt);

    // True when `binding` is THIS exact
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

    // The loop-`else` half of always_returns, factored out because BOTH the
    // For arm and the While arm need it and a second copy could drift. See
    // the definition for the measurement. static, matching its two callers.
    // `body_never_runs` is for a loop whose condition FOLDS FALSE: the body
    // cannot execute, so no `break` written in it can run and the `else` is
    // guaranteed. Defaulted, so the For arm (which has no condition to fold)
    // and every other caller are untouched. It is deliberately a fact about
    // the LOOP HEADER passed in, not something this function derives -- it
    // receives suites, not the While node, and giving it the node would make
    // the For caller pass something it does not have.
    bool loop_else_always_returns(const std::vector<ast::StmtPtr>& body,
                                  const std::vector<ast::StmtPtr>& orelse,
                                  const std::string* narrowed_true_name = nullptr,
                                  bool body_never_runs = false);

    // Task 20's return-path check. Purely syntactic -- it touches no member,
    // no scope, no ClassTable, nothing but the AST shape -- so it can be (and
    // is) called after the function's own body has already been visited,
    // with no ordering hazard either way. static,
    // matching contains_reachable_break right below it for the same reason.
    // See the class-level comment for the exact per-statement rule; "a body
    // always returns if ANY of its statements does" is the fold this
    // recursion performs at every level, mirroring collect_classes' own
    // recursive-then-fold shape.
    bool always_returns(const std::vector<ast::StmtPtr>& body);

    // The name a `while` header narrows TRUTHY for the whole loop body, or
    // nullopt when this loop narrows nothing this checker can rely on.
    //
    // Measured 2026-09-16 against mypy 1.18.1 (three parallel sweeps; see
    // CLAUDE.md). `while c:` narrows `c` to `Literal[True]` for the body, so
    // a guard `if c:` inside it is statically true and a `break` in the
    // branch that guard excludes is UNREACHABLE -- which is why mypy accepts
    // the shapes this compiler used to call `missing return statement`.
    //
    // DELIBERATELY NARROW, and every omission below only RETAINS a false
    // positive this compiler already emits, never silences an error. The
    // asymmetry is the whole design constraint: an evaluator MORE aggressive
    // than mypy's narrowing silences real errors, one LESS aggressive merely
    // keeps false positives.
    //   - The condition must be a BARE NAME. Measured NOT narrowing, so each
    //     must stay excluded: `while c or d:`, `while c == True:`,
    //     `while c != False:`, `while bool(c):`, `while len(s) > 0:`,
    //     `while c > 0:`, `while True:`, `while 1:`. (`while c and d:`,
    //     `while c is True:` and `while not c:` DO narrow and are omitted
    //     only for scope.)
    //   - The name's DECLARED TYPE must be exactly `bool`, and this is
    //     SAFETY-CRITICAL rather than fussy: truthiness narrowing yields a
    //     statically decidable literal for `bool` alone (and `bool | None`,
    //     omitted for scope). Measured, mypy REPORTS for every one of
    //     `c: int`, `c: str`, `c: float`, `c: list[int]`, `c: object`,
    //     `c: int | None` and `c: str | bool`, so narrowing without this
    //     check would silence seven measured errors.
    //   - The body must not REBIND the name anywhere. Any binding kills
    //     mypy's narrowing regardless of the value assigned -- measured,
    //     `c = True` immediately before the guard makes mypy REPORT, because
    //     assigning a bare literal to a `bool`-declared name widens back to
    //     `bool` rather than re-narrowing. That shape prints the "expected"
    //     output under CPython, so it looks fine and is not.
    std::optional<std::string> loop_narrows_truthy(const ast::While& loop);

    // The `while True` arm's "no reachable break" half: true when `body`
    // contains a `break` at any depth EXCEPT inside a nested For/While's own
    // BODY -- a break belonging to a nested loop's body can only ever escape
    // THAT loop, never this one, so recursing into one would over-count. Its
    // ORELSE is the opposite case and IS recursed
    // into: a loop's `else` runs outside that loop's own break scope, so a
    // break there targets the enclosing loop. Recurses into If's body/orelse
    // unconditionally (an `if` is not a loop at all, so a break inside one
    // always still belongs to the enclosing loop).
    //
    // `in_function` is a REAL parameter, not hardcoded, because this is
    // reached from two different contexts that disagree: always_returns and
    // loop_else_always_returns only ever run on a FunctionDef's own body (see
    // always_returns' own comment), so `true` is correct there, but
    // statement_always_leaves's While/For arms are reached from
    // check_suite at MODULE and CLASS scope too, where a `return` is not
    // legal at all -- see the definition's own comment for the measured
    // regression this fixes (in_function=true hardcoded here once silently
    // suppressed a real diagnostic after `while True: return / break` at
    // module scope, where both mypy and CPython reject the `return` outright
    // as a blocking error). `in_loop` is NOT threaded the same way: this
    // scan only ever runs over a loop's own body, so `true` stays correct by
    // construction everywhere it is called from.
    //
    // MUTUALLY RECURSIVE with statement_always_leaves, an edge this comment
    // did not mention before it existed: this function calls
    // statement_always_leaves on its own trailing "does this statement
    // always leave" check, and statement_always_leaves's While/For arms call
    // back into this function on that loop's OWN body. Termination is still
    // guaranteed -- every call in the cycle descends into a STRUCTURALLY
    // SMALLER, NESTED statement list (a nested loop's or `if`'s own body/
    // orelse, never the same list twice), so the recursion is bounded by the
    // AST's finite depth, the same way any tree-shaped mutual recursion
    // terminates.
    // `narrowed_true_name`, when non-null, is the name a `while <name>:`
    // header has narrowed TRUTHY for this body -- see
    // narrowing_guard_verdict for the measured rule and for why the
    // declared type has to be `bool` for it to be set at all.
    bool contains_reachable_break(const std::vector<ast::StmtPtr>& body, bool in_function,
                                  const std::string* narrowed_true_name = nullptr);

    // "Does control ALWAYS leave this branch rather than falling through to
    // the statement after the enclosing `if`?" -- used ONLY by visit(If)'s
    // narrowing join, to decide whether a branch's end-of-branch state is a
    // reachable edge at the merge.
    //
    // DELIBERATELY NOT always_returns, and the two must not be merged: they
    // ask different questions and their SAFE directions are opposite.
    // always_returns answers "does this function body guarantee a return",
    // where answering false wrongly means a spurious "missing return
    // statement" -- so a `break` must NOT count there, since `while True:`
    // with a reachable break is exactly the shape that check treats as
    // skippable. This predicate answers "is the merge reachable from here",
    // where answering false wrongly keeps an UNREACHABLE edge, and an edge
    // that never assigned a narrowed path contributes that path's DECLARED
    // type -- widening a narrowing the surviving branch established into a
    // false TypeError. A `break` and a `continue` both leave the branch just
    // as surely as a `return`, so all three count here.
    //
    // Purely syntactic, non-recursive-into-nested-scopes, same shape as
    // always_returns otherwise: a Return/Break/Continue in the list is a hit;
    // an If counts only when orelse() is NON-EMPTY and BOTH branches leave
    // (so the three terminators may be mixed across the arms); a While counts
    // when its condition is the literal `True` and its body has no reachable
    // break (it never falls through at all); a nested For/While's own BODY is
    // NOT recursed into, because a `break`/`continue` written there belongs to
    // THAT loop and cannot leave this branch, while its ORELSE IS recursed
    // into whenever the nested loop's body has no reachable break, since that
    // `else` then always runs and runs outside the nested loop's own break
    // scope -- the same body-vs-orelse distinction contains_reachable_break
    // draws, for the same reason.
    //
    // THE TWO CONTEXT FLAGS DEFAULT TO TRUE, which is "assume every
    // terminator is legal where it stands" -- exactly the behaviour this
    // predicate had before they existed, so visit(If)'s join (its only
    // caller besides check_suite) is unchanged. check_suite passes the real
    // context instead; see statement_always_leaves for why.
    bool always_leaves_branch(const std::vector<ast::StmtPtr>& body,
                              bool in_function = true, bool in_loop = true,
                              const std::string* narrowed_true_name = nullptr);

    // The per-statement half of always_leaves_branch, extracted so the
    // reachability walk below can ask the question of ONE statement -- "is
    // everything after this statement, in this suite, dead code?" --
    // without re-deriving the rule. always_leaves_branch is now literally an
    // any-of fold over this, and its own contract is UNCHANGED: a hit
    // anywhere in the list still counts, because whatever follows a
    // terminator in the same suite cannot fall through either.
    //
    // The rule itself is documented on always_leaves_branch above; every
    // clause of it lives here now.
    //
    // `in_function`/`in_loop` say whether a `return`, or a `break`/
    // `continue`, is LEGAL PYTHON where this statement stands. They exist
    // because a terminator that CPython refuses to compile must not start an
    // unreachable region: doing so drops the only diagnostic this checker
    // has on a program both oracles reject. Four rows, all measured against
    // mypy 1.18.1 and CPython 3.14.2:
    //
    //   x: int = 0 / break / y: int = "s"
    //     mypy: `"break" outside loop` -- EXIT 2, no bracketed code, and the
    //           line-3 type error never appears (mypy stopped before type
    //           checking; this is a BLOCKING error, not reachability pruning)
    //     CPython: `SyntaxError: 'break' outside loop` -- compile time, so
    //              the file never runs at all
    //   the same with `continue`: `"continue" outside loop`, exit 2;
    //     CPython `SyntaxError: 'continue' not properly in loop`
    //   x: int = 0 / return / y: int = "s"
    //     mypy: `"return" outside function  [misc]`, exit 1
    //     CPython: `SyntaxError: 'return' outside function`
    //   for i in range(2): / class C: / a: int = 0 / break
    //     mypy: `"break" outside loop`, exit 2; CPython: SyntaxError. So a
    //     CLASS BODY resets in_loop even inside a loop -- and `def f(): /
    //     class C: / return` measures the same way, so it resets in_function
    //     too. `def f(): break` (in a function, no loop) is likewise
    //     `"break" outside loop`.
    //
    // Both oracles reject all four, so cythonpp must not go silent on them;
    // at 2997f6f it rejected each one via the type error on the following
    // line, and suppressing that error without this gate would have turned
    // four rejections into four silent acceptances. That cythonpp reports
    // nothing for the stray terminator ITSELF is a separate, PRE-EXISTING
    // gap (a bare module-level `break` with no type error after it is
    // silently accepted at 2997f6f too); closing it belongs to the parser,
    // whose SyntaxError code this pass does not own.
    //
    // The one legal module-scope shape is unaffected and now correct:
    // `x: int = 0 / while True: / x = x + 1 / y: int = "s"` involves no
    // terminator, mypy is CLEAN on it, and 2997f6f reported a false
    // TypeError there that this change removes.
    //
    // MUTUALLY RECURSIVE with contains_reachable_break (see that function's
    // own comment for the termination argument): this function's While/For
    // arms call contains_reachable_break on that loop's own body, and
    // contains_reachable_break's trailing "does this statement always leave"
    // check calls back into this function.
    bool statement_always_leaves(const ast::Stmt& statement, bool in_function = true,
                                 bool in_loop = true,
                                 const std::string* narrowed_true_name = nullptr);

    // THE ONE CHOKE POINT every suite walk goes through, and the whole of
    // this checker's reachability model. It walks each statement exactly as
    // an open-coded `for` loop did before, and additionally, once a statement
    // says statement_always_leaves, treats the REST of the suite -- including
    // every nested subtree of it -- as unreachable.
    //
    // UNREACHABLE MEANS "TYPE-UNCHECKED", NOT "UNVISITED", and that is a
    // measurement against mypy 1.18.1, not a convenience. mypy's semantic
    // analyzer runs over unreachable code and its type checker does not:
    //
    //   def f() -> int:
    //       return 0
    //       print(nope_not_defined)
    //   print(f())
    //
    //   $ mypy --strict --no-color-output --no-error-summary probe.py
    //   probe.py:3: error: Name "nope_not_defined" is not defined  [name-defined]
    //
    // while the same position given a bad attribute, a bad call arity and a
    // bad argument type is silent (`Success` -- and the CONTROL with those
    // three statements made reachable reports all three, so the silence is
    // reachability and not unreportability). So the walk must CONTINUE: names
    // must still bind and resolve. Dropping the walk instead is wrong in both
    // directions at once -- it loses the NameError above, and it invents one
    // for a name bound only in unreachable code and read from statically
    // reachable code, which BOTH oracles accept:
    //
    //   def f(c: bool) -> None:
    //       if c:
    //           return
    //           x: int = 1
    //       print(x)
    //   print("module ran")
    //
    //   $ mypy --strict ... -> Success: no issues found in 1 source file
    //   $ python probe.py  -> module ran   (exit 0)
    //
    // WHAT IS SUPPRESSED IS EXACTLY DiagnosticKind::TypeCheckerTypeError,
    // via DiagnosticSuppression on the sink -- NOT the code string
    // "TypeError". That distinction is the whole point and it cost a round to
    // learn: cythonpp spells "TypeError" for judgements mypy's semantic
    // analyzer owns as well as for judgements its type checker owns, and
    // mypy's semantic analyzer DOES run here. Suppressing the string silently
    // accepted a redefinition after a `return` (mypy [no-redef]), a duplicate
    // parameter name in unreachable code (mypy exit 2 AND a CPython
    // compile-time SyntaxError, so the file never runs at any reachability),
    // and every malformed annotation ([valid-type], [type-arg]) -- nine
    // measured classes. diagnostic_kind.h carries the per-class measurements
    // and the test a new report site should apply.
    //
    // NameError stays because mypy reports it there (above).
    // NotImplementedError and OverflowError stay because neither is a mypy
    // type judgement at all -- both are THIS compiler's own capability claims
    // ("cannot model this construct", "this literal does not fit 64 bits"),
    // unreachable code still has to be emitted as C++, and neither is silent
    // acceptance, so keeping them cannot violate the union rule.
    //
    // NARROWING DOES NOT ESCAPE. The state is snapshotted at the moment the
    // suite goes unreachable and restored before returning, so the enclosing
    // construct's own end-of-suite snapshot sees the state as of the
    // terminator -- which is where the suite genuinely ends. Without this a
    // narrowing recorded by an unreachable statement reaches REACHABLE code
    // through the enclosing loop join; measured at 2997f6f on
    // `n: object = object()` / `n = 7` / `while f:` / `if f: break else:
    // break` / `n = "s"` / `k: int = n`, which drew `incompatible types in
    // assignment (expression has type "int | str", variable has type "int")`
    // on the reachable `k: int = n` while mypy --strict said `Success` and
    // CPython printed `7` twice.
    void check_suite(const std::vector<ast::StmtPtr>& body);

    void report(const ast::Node& at, DiagnosticKind kind, std::string message);
    void report_incompatible_assignment(const ast::Node& at, const Type& value_type,
                                        const Type& target_type, const char* target_label);

    diagnostics::DiagnosticSink& sink_;
    ScopeStack scopes_;
    ClassTable classes_;
    TypeMap types_;
    // The narrowing state of the code currently being checked -- driven here
    // (assign, kill, join, reset) and read by ExpressionTyper. Declared ABOVE
    // typer_ because typer_'s constructor takes a reference to it.
    NarrowingMap narrowings_;
    ExpressionTyper typer_;

    // Whether the suite currently being walked is lexically inside a
    // function body, and inside a loop body -- the two facts
    // statement_always_leaves needs to tell a real terminator from a
    // statement CPython refuses to compile. Both start false, which is
    // module scope. Set by FlagGuard (in the .cpp) around each body walk:
    // a FunctionDef body sets in_function=true and in_loop=FALSE (a `break`
    // cannot cross a `def`), a ClassDef body sets BOTH false (measured: a
    // class body resets each), a While/For BODY sets in_loop=true, and a
    // While/For ORELSE inherits unchanged, because an `else` runs outside
    // its own loop's break scope.
    bool in_function_body_ = false;
    bool in_loop_body_ = false;

    // One recorded module-scope definition, for the collision rule
    // scan_top_level_names implements. `at_flat_top_level` is false for a
    // definition found inside an `if`/`while`/`for` block, which is the ONE
    // fact the rule needs beyond the line and the kind -- see that function.
    struct TopLevelDefinition {
        int line = 0;
        bool is_class = false;
        bool at_flat_top_level = false;
    };

    // Populated by scan_top_level_names; see its comment.
    std::map<std::string, TopLevelDefinition> top_level_definitions_;
    std::set<const ast::Node*> collided_top_level_;

    // Every nested `def` that does NOT sit directly in its enclosing
    // function's body -- i.e. one inside an `if`/`while`/`for` at any depth
    // within that body. mypy's conditional-function-definition allowance
    // turns on exactly this fact, and it is NOT scope-limited the way
    // scan_top_level_names' `at_flat_top_level` is: the same rule holds at
    // module scope, in a class body, and inside a `def`.
    //
    // Filled in visit(FunctionDef) by the same for_each_flat_statement walk
    // scan_top_level_names uses on the module body, run once per function
    // body just before that body is walked -- so by the time the nested
    // def's own visit(FunctionDef) reaches the binding site, its enclosing
    // body's scan has already classified it. Keyed by node ADDRESS, never
    // cleared: the AST outlives the check, two bodies cannot contain the same
    // FunctionDef node, and a stale entry is therefore impossible rather than
    // merely unlikely.
    std::set<const ast::FunctionDef*> conditional_defs_;

    // Every MODULE-and-CLASS-level class the declaration pass declared, in
    // declaration order. The member-collection phase walks this to
    // pre-collect every class's members before the first statement is
    // checked; a function-local or collision-losing class is NOT here
    // (declare_isolated_class owns those, and each is pre-collected by its
    // own visit(ClassDef) when the walk reaches it).
    std::vector<ClassDeclaration> declared_classes_;

    // Which ClassDefs pre_collect_class_body has already run for, so
    // visit(ClassDef) does not run it a SECOND time for a class the
    // module-wide phase already covered -- doing so would re-resolve every
    // annotation in the body through AnnotationResolver and report each bad
    // one twice.
    std::set<const ast::ClassDef*> pre_collected_;

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
    // NOTE: "has an entry here" no longer implies "sits flat in the module
    // body" -- collect_signatures now recurses through control flow, so a def
    // under an `if`/`while`/`for` gets an entry too. Do not add a rule that
    // keys off this map's presence to mean "flat"; it would get the wrong
    // answer for a conditional def. (This is the same trap that forced
    // class_method_signatures_ below to be a separate map rather than folded
    // into this one -- see its own comment.)
    std::map<const ast::FunctionDef*, Type> top_level_signatures_;

    // Pre_collect_class_body's own resolved-signature cache, one
    // entry per METHOD (a direct FunctionDef in a class body) -- the exact
    // analogue of top_level_signatures_ above, kept as a SEPARATE map (rather
    // than folded into it) because that map's own contract explicitly reads
    // "every top-level FunctionDef", and visit(FunctionDef)'s cached branch
    // used to rely on "found in top_level_signatures_" implying "is not a
    // method" (see its own self-exemption comment, now corrected). A method
    // resolved here is looked up by cached_signature_for, which checks both
    // maps.
    std::map<const ast::FunctionDef*, Type> class_method_signatures_;

    // Pre_collect_class_body's resolved-annotation cache for a
    // class-body-DIRECT AnnAssign (never a nested one, matching that
    // function's own module.body()-only-style simplification) -- the exact
    // analogue of module_level_annotations_, so visit(AnnAssign)'s
    // class-body branch reuses this Type via bind_resolved_annotation
    // instead of invoking AnnotationResolver (and possibly double-reporting
    // a bad annotation) a second time.
    std::map<const ast::AnnAssign*, Type> class_body_annotation_types_;

    // The same cache, for the METHOD-level `self.x: T` form, populated by
    // declare_self_attribute_placeholder and consumed by visit(AnnAssign)'s
    // non-Name-target path. Kept separate from class_body_annotation_types_
    // because the two are populated by different sub-passes over different
    // statement shapes and consumed on different branches -- and because
    // only entries this map holds are safe to reuse on the non-Name path,
    // which also serves `xs[0]: int` and `other.x: int` targets that are
    // never pre-resolved at all.
    //
    // Its existence is what lets the eager scan install the REAL annotated
    // type rather than an absorbing Unknown (see
    // declare_self_attribute_placeholder): eager resolution alone would
    // report a bad annotation once per pass, and the cache turns the second
    // pass into a lookup. Only annotations the scan actually resolved are in
    // here -- one it SKIPPED (the name is already declared on this class) is
    // resolved by visit(AnnAssign) as before, so either way a bad annotation
    // is resolved, and reported, exactly once.
    std::map<const ast::AnnAssign*, Type> self_annotation_types_;

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

    // One frame per function body currently being
    // walked, innermost last (pushed and popped by LocalClassAliasGuard, in
    // the .cpp, alongside the FunctionScopeGuard that pushes that body's own
    // ScopeKind::Function). A function-local ClassDef is declared into
    // ClassTable under a synthetic ISOLATED qualified name -- which is what
    // keeps two same-named local classes in different functions from
    // overwriting each other, and keeps neither from leaking to later
    // module-level code -- and registers a scope-limited alias from its BARE
    // source-level name to that isolated name in the innermost frame here,
    // so bare-name constructor dispatch (ExpressionTyper::type_of_name_call's
    // classes_.is_class(identifier) lookup, which has no scope awareness of
    // its own), attribute lookup and annotation resolution all resolve it --
    // but only from inside the function that declares it. Empty at module
    // level, which is exactly why a module-level `L()` after a `def f` that
    // declares `class L` still reports the NameError mypy reports for it.
    std::vector<LocalClassAliasFrame> local_class_alias_frames_;

    // The names in the CURRENT scope's body whose bare-empty-`list`/`dict`
    // first assignment seeds a mypy PARTIAL CONTAINER type the per-scope scan
    // has proven is RESOLVED before it is ever read -- so the eager
    // `need type annotation` report at that assignment must be suppressed and
    // a Binding::partial_container recorded in its place.
    //
    // Computed by the file-local resolvable_container_partials() scan and
    // installed at FOUR call sites, not three: visit(Module), BOTH
    // visit(FunctionDef) branches (the ordinary one and the
    // report-and-return one a method with no parameters takes), and
    // visit(ClassDef). Wiring only some of them is the single most likely way
    // to half-land this rule -- the module-scope accepting tests pass with
    // just the first -- which is why there is a per-scope accepting test for
    // each. Saved and restored by ResolvablePartialsGuard (in the .cpp)
    // around every scope push but the module's, which is outermost and has
    // nothing to restore to.
    std::set<std::string> resolvable_container_partials_;
};

} // namespace cythonpp::domain::semantic

#endif // CYTHONPP_DOMAIN_SEMANTIC_TYPE_CHECKER_H
