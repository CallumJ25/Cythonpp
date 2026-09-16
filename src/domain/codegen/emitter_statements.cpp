#include "domain/codegen/emitter.h"

#include <algorithm>
#include <cstddef>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "domain/ast/ann_assign.h"
#include "domain/ast/assign.h"
#include "domain/ast/break.h"
#include "domain/ast/class_def.h"
#include "domain/ast/continue.h"
#include "domain/ast/expr_stmt.h"
#include "domain/ast/for.h"
#include "domain/ast/function_def.h"
#include "domain/ast/if.h"
#include "domain/ast/name.h"
#include "domain/ast/pass.h"
#include "domain/ast/recursive_visitor.h"
#include "domain/ast/return.h"
#include "domain/ast/while.h"
#include "domain/codegen/cpp_type_name.h"
#include "domain/codegen/name_mangler.h"
#include "domain/diagnostics/diagnostic_sink.h"
#include "domain/semantic/annotation_resolver.h"
#include "domain/semantic/class_lookup.h"
#include "domain/semantic/type.h"
#include "domain/semantic/type_compatibility.h"
#include "domain/semantic/type_name.h"

namespace cythonpp::domain::codegen {
namespace {

// HOLE (B), part 1: AnnotationResolver needs a ClassLookup, and Emitter has
// none -- Tasks 5/6 never gave it one, and threading a real
// semantic::ClassTable through here would mean either constructing a second,
// throwaway one from nothing (ClassTable is populated by TypeChecker's own
// three-phase walk, which the emitter does not repeat) or widening Emitter's
// constructor to take one from its caller, a change with real blast radius
// for a slice that never needs it.
//
// This stand-in answers "not a class" unconditionally, and that answer
// cannot produce a WRONG Type for anything this slice actually admits.
// AnnotationResolver::resolve_name (annotation_resolver.cpp) checks
// builtin_type_kind FIRST and returns straight away for int/float/bool/str/
// None/object -- the entire scalar surface cpp_type_name recognises -- with
// no call to ClassLookup at all. ClassLookup is consulted only once
// builtin_type_kind has already said no, i.e. only for a name that could be
// a user-defined class. Such a name resolves here to a NameError (discarded,
// see below) and Type::unknown(), where the real ClassTable would have
// resolved it to Type::class_of(...) instead -- but cpp_type_name has no
// mapping for TypeKind::Class either way, so both answers are refused
// identically by whichever caller passed the resolved Type to cpp_type_name.
// A class annotation is outside this slice regardless of which ClassLookup
// answered the question.
class NullClassLookup : public semantic::ClassLookup {
public:
    bool is_class(const std::string&) const override { return false; }
    std::vector<semantic::Type> bases_of(const std::string&) const override { return {}; }
    std::string canonical_name(const std::string& name) const override { return name; }
};

// HOLE (B), part 2: resolves one annotation Expr to a Type, reporting through
// a SCRATCH sink that is discarded rather than the emitter's own sink_.
//
// The annotation reaching this function already passed TypeChecker cleanly:
// Emitter only ever runs on a module the checker accepted with no errors
// (Fixture::build's own front_sink/check_sink asserts enforce this for every
// test, and the real compile pipeline gates codegen on the same thing), so a
// diagnostic from re-resolving that exact annotation here would mean one of
// two things, and neither belongs on the emitter's real sink:
//   1. It duplicates a diagnostic TypeChecker already reported once -- which
//      cannot actually happen for a module that reached codegen, but would
//      be a genuine double-report if it somehow did.
//   2. It is an ARTIFACT of NullClassLookup's own simplification (a real
//      user class the true ClassTable would have resolved, that this stand-
//      in cannot) -- a diagnostic about the compiler's OWN modelling choice,
//      not a defect in the user's program, and reporting it to the user
//      would be actively misleading.
// Swallowing a genuine new error is not a risk either way: the real signal
// for "this annotation's type is unsupported" is the nullopt cpp_type_name
// returns for the resolved Type, and every caller turns that into its own
// correctly-worded refuse() against the REAL sink (e.g. "an unsupported
// parameter type"), so the user still gets exactly one diagnostic, just from
// the emitter's own reporting path rather than this scratch resolution.
//
// FINAL-REVIEW cleanup: a wrapper, Emitter::cpp_type_name_of_annotation,
// used to spell that "resolve then name" pair in one call. Task 7's fix round
// rewrote write_signature to need the resolved Type itself (to seed
// return_type_ and function_declared_), not just its C++ spelling, and the
// wrapper was orphaned -- declared, defined, and called from nowhere in src/
// or tests/. Deleted; every caller resolves and names in two steps now.
//
// A MEMBER function (Emitter::resolve_annotation_type, defined below rather
// than a free function local to this translation unit) so emitter_module.cpp
// can resolve a module-level AnnAssign's annotation identically -- the
// NullClassLookup stand-in stays a file-local implementation detail here,
// since only this function ever constructs one.

// CRITICAL FIX (post-review round 1): the runtime spelling of the explicit
// conversion needed to raise a value of kind `from` into a variable/return
// slot of kind `to`, or nullptr when none is needed. Reuses
// semantic::numeric_rank -- the numeric tower's single shared authority
// (type_compatibility.h's own comment: "shared with the operator rules") --
// rather than hand-rolling a second copy of Bool <: Int <: Float here, which
// is exactly the kind of duplication that let two copies of this tower
// drift apart elsewhere in this codebase.
//
// Only WIDENING (from_rank < to_rank) ever needs a conversion; equal ranks
// (including two kinds outside the tower entirely, e.g. Str to Str, where
// both ranks are 0) emit nothing, matching every existing test's plain
// `emit_expr` output. A narrowing pair (e.g. to_rank < from_rank) can only
// reach this function for a program the checker would already have
// rejected -- is_subtype's numeric-tower rule is a total order in the
// widening direction only -- so it is treated the same as "no relationship",
// not as a case to guess a conversion for.
//
// The only two reachable `to` kinds are Int and Float: `to_rank` is nonzero
// (checked below) and STRICTLY GREATER than `from_rank`, and Bool -- rank 1,
// the tower's own floor -- can never be a strict upper bound on anything.
const char* numeric_widening_function(semantic::TypeKind from, semantic::TypeKind to) {
    const int from_rank = semantic::numeric_rank(from);
    const int to_rank = semantic::numeric_rank(to);
    if (from_rank == 0 || to_rank == 0 || from_rank >= to_rank) {
        return nullptr;
    }
    return to == semantic::TypeKind::Int ? "py::to_int" : "py::to_float";
}

// Every Name READ anywhere in an expression sub-tree, in source order.
// A RecursiveVisitor (not a plain Visitor) precisely because the default
// "recurse into everything else" is what is wanted here: a Name nested in a
// call argument, an operand, or a comparison chain is still a read, and a
// node this collector forgets costs a MISSED read rather than a wrong answer
// -- and every expression node this slice can emit is reachable from the
// defaults already.
class NameReadCollector : public ast::RecursiveVisitor {
public:
    using ast::RecursiveVisitor::visit;
    void visit(const ast::Name& node) override { names.push_back(&node); }

    std::vector<const ast::Name*> names;
};

// The name an assignment statement binds, or nullptr when the statement binds
// no plain name at all (a subscript/attribute target, a non-ASCII identifier,
// or a bare `x: int` with no value -- which DECLARES in C++ but binds nothing
// in Python, so it must not count as an assignment for definite-assignment
// purposes).
const ast::Name* bound_name_of(const ast::Stmt& statement) {
    const ast::Expr* target = nullptr;
    if (const auto* assign = dynamic_cast<const ast::Assign*>(&statement)) {
        target = &assign->target();
    } else if (const auto* ann_assign = dynamic_cast<const ast::AnnAssign*>(&statement)) {
        if (!ann_assign->has_value()) {
            return nullptr;
        }
        target = &ann_assign->target();
    } else {
        return nullptr;
    }
    return dynamic_cast<const ast::Name*>(target);
}

} // namespace

semantic::Type Emitter::resolve_annotation_type(const ast::Expr& annotation) const {
    NullClassLookup classes;
    diagnostics::DiagnosticSink discarded;
    semantic::AnnotationResolver resolver(classes, discarded);
    return resolver.resolve(annotation);
}

const semantic::Type* Emitter::declared_type_for_assignment(const ast::Expr& target,
                                                            const ast::Expr& value,
                                                            const semantic::Type* declared) const {
    if (declared != nullptr) {
        return declared;
    }
    const semantic::Type* target_type = type_of(target);
    if (target_type != nullptr) {
        return target_type;
    }
    return type_of(value);
}

// FINAL-REVIEW CRITICAL 3, half one. See emitter.h for why this recurses.
bool Emitter::collect_scope_variables(const std::vector<ast::StmtPtr>& body,
                                      const std::map<std::string, semantic::Type>& already_declared,
                                      std::vector<ScopeVariable>& variables) {
    for (const ast::StmtPtr& stmt : body) {
        // An `if`/`while` body is the SAME Python scope as the statement list
        // containing it, so its assignments belong to this collection. A
        // nested def or class is a different scope and is deliberately not
        // descended into (both are refused by this stage regardless).
        if (const auto* branch = dynamic_cast<const ast::If*>(stmt.get())) {
            if (!collect_scope_variables(branch->body(), already_declared, variables) ||
                !collect_scope_variables(branch->orelse(), already_declared, variables)) {
                return false;
            }
            continue;
        }
        if (const auto* loop = dynamic_cast<const ast::While*>(stmt.get())) {
            if (!collect_scope_variables(loop->body(), already_declared, variables) ||
                !collect_scope_variables(loop->orelse(), already_declared, variables)) {
                return false;
            }
            continue;
        }

        const ast::Expr* target = nullptr;
        const ast::Expr* value = nullptr;
        const semantic::Type* declared_ptr = nullptr;
        semantic::Type declared_storage;
        if (const auto* assign = dynamic_cast<const ast::Assign*>(stmt.get())) {
            target = &assign->target();
            value = &assign->value();
        } else if (const auto* ann_assign = dynamic_cast<const ast::AnnAssign*>(stmt.get())) {
            target = &ann_assign->target();
            declared_storage = resolve_annotation_type(ann_assign->annotation());
            declared_ptr = &declared_storage;
            if (ann_assign->has_value()) {
                value = &ann_assign->value();
            }
        } else {
            continue;
        }

        const auto* name = dynamic_cast<const ast::Name*>(target);
        if (name == nullptr || !is_manglable_identifier(name->identifier())) {
            // Refused later, once, by the ordinary per-statement walk, which
            // has the correctly-worded diagnostic for each case.
            continue;
        }
        const std::string mangled = mangle(name->identifier());
        if (already_declared.find(mangled) != already_declared.end()) {
            continue; // A parameter: already declared by the signature.
        }
        const auto seen = std::find_if(variables.begin(), variables.end(),
                                       [&mangled](const ScopeVariable& variable) {
                                           return variable.mangled == mangled;
                                       });
        if (seen != variables.end()) {
            continue; // Not the first assignment; already collected.
        }

        // FINAL-REVIEW IMPORTANT 5: a bare `x: float` (no value) has no value
        // Expr for declared_type_for_assignment's last fallback link, so that
        // link is skipped and the annotation -- always present on an AnnAssign
        // -- is used directly. Collecting it HERE is what fixes the
        // module-vs-function inconsistency the review found: visit(AnnAssign)
        // used to write a lone `;` and record nothing, so a later `x = 1`
        // declared cy_x from the VALUE (py::int_) and the `x = 2.5` after it
        // did not compile. The module prelude always did record it; now both
        // scopes go through this one collector.
        const semantic::Type* type =
            value != nullptr ? declared_type_for_assignment(*target, *value, declared_ptr)
                             : declared_ptr;
        if (type == nullptr) {
            refuse(*target, "an assignment whose type is unknown");
            return false;
        }
        if (!cpp_type_name(*type).has_value()) {
            refuse(*target, "an assignment of an unsupported type");
            return false;
        }
        variables.push_back(ScopeVariable{name->identifier(), mangled, *type});
    }
    return true;
}

void Emitter::check_reads(const ast::Expr& expr, const std::set<std::string>& locals,
                          const std::set<std::string>& bound,
                          std::string_view unbound_phrase) {
    NameReadCollector collector;
    expr.accept(collector);
    for (const ast::Name* name : collector.names) {
        const std::string& identifier = name->identifier();
        if (locals.count(identifier) == 0 || bound.count(identifier) != 0) {
            continue;
        }
        refuse(*name, "a read of '" + identifier + "', which " +
                          std::string(unbound_phrase) + ",");
        return;
    }
}

// FINAL-REVIEW CRITICAL 3, half two. See emitter.h for the full argument.
bool Emitter::check_definite_assignment(const std::vector<ast::StmtPtr>& body,
                                        const std::set<std::string>& locals,
                                        std::set<std::string>& bound,
                                        std::string_view unbound_phrase) {
    for (const ast::StmtPtr& stmt : body) {
        if (failed_) {
            return false;
        }
        if (const auto* expression = dynamic_cast<const ast::ExprStmt*>(stmt.get())) {
            check_reads(expression->value(), locals, bound, unbound_phrase);
            continue;
        }
        if (const auto* returned = dynamic_cast<const ast::Return*>(stmt.get())) {
            if (returned->has_value()) {
                check_reads(returned->value(), locals, bound, unbound_phrase);
            }
            return true;
        }
        if (dynamic_cast<const ast::Break*>(stmt.get()) != nullptr ||
            dynamic_cast<const ast::Continue*>(stmt.get()) != nullptr) {
            return true;
        }
        if (const auto* branch = dynamic_cast<const ast::If*>(stmt.get())) {
            check_reads(branch->condition(), locals, bound, unbound_phrase);
            std::set<std::string> then_bound = bound;
            const bool then_leaves =
                check_definite_assignment(branch->body(), locals, then_bound, unbound_phrase);
            std::set<std::string> else_bound = bound;
            const bool else_leaves =
                check_definite_assignment(branch->orelse(), locals, else_bound, unbound_phrase);
            if (then_leaves && else_leaves) {
                return true;
            }
            // An arm that always leaves cannot be the one that falls through,
            // so it contributes nothing to intersect -- the other arm's
            // bindings stand alone. Only when BOTH fall through is the
            // intersection the answer.
            if (then_leaves) {
                bound = else_bound;
            } else if (else_leaves) {
                bound = then_bound;
            } else {
                std::set<std::string> merged;
                for (const std::string& name : then_bound) {
                    if (else_bound.count(name) != 0) {
                        merged.insert(name);
                    }
                }
                bound = merged;
            }
            continue;
        }
        if (const auto* loop = dynamic_cast<const ast::While*>(stmt.get())) {
            check_reads(loop->condition(), locals, bound, unbound_phrase);
            // A loop body may run ZERO times, so nothing it binds is
            // definitely bound afterwards -- and its own reads are checked
            // against the state on the FIRST iteration, which is exactly the
            // state CPython would raise UnboundLocalError from. Its `else`
            // clause is checked the same way and likewise contributes nothing
            // (it does not run when the loop is left by `break`).
            std::set<std::string> body_bound = bound;
            check_definite_assignment(loop->body(), locals, body_bound, unbound_phrase);
            std::set<std::string> else_bound = bound;
            check_definite_assignment(loop->orelse(), locals, else_bound, unbound_phrase);
            continue;
        }
        // A nested def or class is a different scope whose body runs later (or
        // never); this stage refuses both anyway. A `for` is refused too.
        if (dynamic_cast<const ast::FunctionDef*>(stmt.get()) != nullptr ||
            dynamic_cast<const ast::ClassDef*>(stmt.get()) != nullptr ||
            dynamic_cast<const ast::For*>(stmt.get()) != nullptr ||
            dynamic_cast<const ast::Pass*>(stmt.get()) != nullptr) {
            continue;
        }
        // Whatever remains is an assignment form: its VALUE is evaluated
        // before its target binds, so `x = x + 1` reads the OLD x.
        if (const auto* assign = dynamic_cast<const ast::Assign*>(stmt.get())) {
            check_reads(assign->value(), locals, bound, unbound_phrase);
        } else if (const auto* ann_assign = dynamic_cast<const ast::AnnAssign*>(stmt.get())) {
            if (ann_assign->has_value()) {
                check_reads(ann_assign->value(), locals, bound, unbound_phrase);
            }
        }
        if (const ast::Name* target = bound_name_of(*stmt)) {
            bound.insert(target->identifier());
        }
    }
    return false;
}

void Emitter::write_indent() {
    for (int level = 0; level < indent_; ++level) {
        write("  ");
    }
}

void Emitter::write_line(std::string_view text) {
    write_indent();
    write(text);
    write("\n");
}

void Emitter::emit_suite(const std::vector<ast::StmtPtr>& body) {
    ++indent_;
    for (const ast::StmtPtr& statement : body) {
        statement->accept(*this);
    }
    --indent_;
}

std::optional<std::string> Emitter::emit_statement_for_test(const ast::Stmt& statement) {
    out_.clear();
    failed_ = false;
    statement.accept(*this);
    if (failed_) {
        return std::nullopt;
    }
    return out_;
}

void Emitter::emit_value_widened(const ast::Expr& value, const semantic::Type& target) {
    const semantic::Type* value_type = type_of(value);
    const char* widen =
        value_type != nullptr ? numeric_widening_function(value_type->kind, target.kind) : nullptr;
    // DECISION 0 BACKSTOP, 2026-09-16. A value only fits a DECLARED C++ slot
    // when the two spell the same C++ type, or when a py::to_int/py::to_float
    // widening call raises it into one; anything else emits an expression the
    // slot has no `operator=`/parameter/return conversion for, which is
    // `no viable overloaded '='` at clang++ -- a .cpp on disk from a run that
    // exited 0 and reported nothing. Refusing here makes that third state
    // unreachable from EVERY one of this function's three callers (an
    // assignment initializer, a `return` value, and a call argument) at once,
    // and it stays a backstop rather than the primary defence: a program whose
    // value genuinely does not fit its declared type is one mypy rejects, so
    // the semantic layer is what SHOULD report, with a far better message.
    //
    // It exists because the semantic layer is measurably not airtight here.
    // Measured 2026-09-16, both mypy-rejected and both silent at 58b5ef5:
    // `x = 1` / `x: float = 2.5` is mypy `Name "x" already defined on line 1
    // [no-redef]`, and `if c: s = "a"` / `s = 5` was the block-first shape the
    // same day's pre_bind_assignment_targets fix closes. The first is a
    // pre-existing gap in a DIFFERENT mechanism (Phase 2 binds every
    // module-level annotation before any assignment is walked, so the
    // collision is never seen) and is deliberately left open -- it is a
    // missed error, the safe direction -- but it must not reach disk as
    // uncompilable C++, and this is what stops it.
    //
    // Compared by C++ SPELLING rather than through is_subtype, deliberately:
    // the question is whether the emitted text compiles, and two types that
    // share a spelling always do while two that do not never do. nullopt
    // compares equal to nullopt, which costs nothing -- an unrepresentable
    // type is already refused at its declaration, before any value reaches
    // this function.
    if (widen == nullptr && value_type != nullptr &&
        cpp_type_name(*value_type) != cpp_type_name(target)) {
        refuse(value, "a value of type \"" + semantic::type_name(*value_type) +
                          "\" where \"" + semantic::type_name(target) + "\" is required");
        return;
    }
    if (widen != nullptr) {
        write(widen);
        write("(");
    }
    emit_expr(value);
    if (widen != nullptr) {
        write(")");
    }
}

void Emitter::visit(const ast::ExprStmt& node) {
    write_indent();
    emit_expr(node.value());
    write(";\n");
}

// A lone `;` rather than nothing at all, so an otherwise-empty suite still
// contains a statement and the emitted braces are never empty in a way that
// reads as a mistake.
void Emitter::visit(const ast::Pass& node) {
    (void)node;
    write_line(";");
}

// FINAL-REVIEW CRITICAL 3: this NEVER declares any more, at either scope.
// Every variable a scope assigns is declared once, up front, by
// collect_scope_variables (the module's file-scope prelude, or the prologue
// visit(FunctionDef) writes), so by the time any assignment is emitted its
// target already has a declaration at SCOPE level -- which is where Python's
// own per-function/per-module scoping puts it. Declaring at the first
// assignment instead gave the C++ variable BLOCK scope whenever that first
// assignment sat inside an `if`/`while`.
void Emitter::emit_assignment(const ast::Expr& target, const ast::Expr& value,
                              const semantic::Type* declared) {
    const auto* name = dynamic_cast<const ast::Name*>(&target);
    if (name == nullptr) {
        refuse(target, "an assignment to something other than a plain name");
        return;
    }
    if (!is_manglable_identifier(name->identifier())) {
        refuse(target, "a non-ASCII identifier");
        return;
    }
    const std::string mangled = mangle(name->identifier());

    // CRITICAL FIX (post-review round 1): a REASSIGNMENT'S target type is
    // whatever the local was FIRST declared as, not whatever this
    // particular value's own type happens to be -- `x: float = 1.0` then a
    // later plain `x = 2` is mypy-clean (2 widens into the DECLARED float),
    // and the C++ variable is still `py::float_`, so the second statement
    // must widen against Float even though `2` on its own types as Int. A
    // function-local lookup wins over the `declared`/type_of fallback chain
    // below for exactly this reason.
    //
    // TASK 8 FIX: at module level function_declared_ is empty (it exists
    // only for the currently-emitting function's body), but a module-level
    // reassignment needs the identical treatment -- `x: float = 1` then a
    // later module-level `x = 2` must ALSO widen against the declared float,
    // or the emitted global `py::float_ cy_x;` is assigned a bare
    // `py::int_(2)`, which does not compile. module_declared_ (populated by
    // emit_module's file-scope prelude before main() ever runs) is that
    // record's module-scope counterpart.
    const semantic::Type* existing = nullptr;
    const std::map<std::string, semantic::Type>& declared_here =
        at_module_level_ ? module_declared_ : function_declared_;
    {
        const auto it = declared_here.find(mangled);
        if (it != declared_here.end()) {
            existing = &it->second;
        }
    }
    const semantic::Type* type = existing;
    if (type == nullptr) {
        type = declared_type_for_assignment(target, value, declared);
    }
    if (type == nullptr) {
        refuse(target, "an assignment whose type is unknown");
        return;
    }
    const std::optional<std::string> cpp = cpp_type_name(*type);
    if (!cpp.has_value()) {
        refuse(target, "an assignment of an unsupported type");
        return;
    }

    write_indent();
    write(mangled);
    write(" = ");
    // CRITICAL FIX (post-review round 1): the initializer must be WIDENED to
    // `*type` when its own static type is a proper numeric-tower subtype of
    // it (see emit_value_widened's own comment) -- `emit_expr(value)` alone
    // reproduced the value's OWN type, e.g. `py::int_(1)` for an `x: float =
    // 1` declaration whose variable is `py::float_`, which does not compile.
    emit_value_widened(value, *type);
    write(";\n");
}

void Emitter::visit(const ast::Assign& node) {
    emit_assignment(node.target(), node.value(), nullptr);
}

void Emitter::visit(const ast::AnnAssign& node) {
    if (!node.has_value()) {
        // A bare `x: int` declares a name without binding it. Python binds
        // nothing at all, so there is no value to emit and no C++ statement
        // that corresponds; the scope's own declaration prologue
        // (collect_scope_variables) has already declared it -- at BOTH
        // scopes now, which is FINAL-REVIEW IMPORTANT 5: the module prelude
        // always did, and a function body did not, so the identical program
        // compiled at module level and did not inside a `def`.
        write_line(";");
        return;
    }
    // FIX: the type-resolution fallback chain's `declared` slot must be the
    // ANNOTATION's own type, not type_of(node.target()) -- TypeMap never
    // types an assignment target Name (see emitter.h's cross-task note and
    // type_map.h's own comment), so that call always returns nullptr and the
    // chain silently fell all the way through to type_of(value) instead.
    // That is not merely imprecise: `x: float = 1` types its value (the
    // literal `1`) as Int, so the fallback would declare `cy_x` as
    // `py::int_`, and a mypy-clean, CPython-clean follow-on line like
    // `x = 2.5` (an ordinary float re-assignment, valid because the DECLARED
    // type is float) would then emit `cy_x = py::float_(2.5);` against a
    // variable declared `py::int_` -- which does not compile, since
    // py::int_ has no assignment operator taking a py::float_. Resolving the
    // annotation directly closes this at the source rather than leaving the
    // fallback to guess from the value.
    const semantic::Type declared_type = resolve_annotation_type(node.annotation());
    emit_assignment(node.target(), node.value(), &declared_type);
}

void Emitter::visit(const ast::If& node) {
    write_indent();
    write("if (py::truthy(");
    emit_expr(node.condition());
    write(")) {\n");
    // FINAL-REVIEW IMPORTANT 4: see in_conditional_block_'s own comment.
    // Saved and restored rather than merely set, for the same reason
    // loop_has_else_ is: blocks nest, and the statement AFTER an `if` is back
    // at whatever level the `if` itself sat at.
    const bool outer_conditional = in_conditional_block_;
    in_conditional_block_ = true;
    emit_suite(node.body());
    if (node.orelse().empty()) {
        in_conditional_block_ = outer_conditional;
        write_line("}");
        return;
    }
    write_line("} else {");
    emit_suite(node.orelse());
    in_conditional_block_ = outer_conditional;
    write_line("}");
}

// Python runs a loop's else clause ONLY when the loop was not left by break,
// so an else needs a flag to distinguish the two exits. A loop with no else
// emits no flag: the common case stays simple.
void Emitter::visit(const ast::While& node) {
    const bool has_else = !node.orelse().empty();
    const std::string flag = "_cy_broke_" + std::to_string(loop_depth_);
    if (has_else) {
        write_line("{");
        ++indent_;
        write_line("bool " + flag + " = false;");
    }
    write_indent();
    write("while (py::truthy(");
    emit_expr(node.condition());
    write(")) {\n");
    ++loop_depth_;
    // HOLE (A): loop_has_else_ is what visit(Break) below reads to decide
    // whether a `_cy_broke_<depth>` flag exists to set. Saved and restored
    // around the BODY exactly as at_module_level_ is around a function's
    // body in visit(FunctionDef), and for the identical reason: loops nest.
    // An inner loop with no else, nested inside an outer loop that has one,
    // must still emit a bare `break;` for ITS OWN break statements -- if
    // loop_has_else_ merely stayed set from the outer loop, the inner
    // loop's break would wrongly reference the outer loop's flag (or worse,
    // the wrong depth number entirely) instead of leaving the inner loop
    // named by nothing at all.
    const bool outer_loop_has_else = loop_has_else_;
    const bool outer_conditional = in_conditional_block_;
    loop_has_else_ = has_else;
    in_conditional_block_ = true;
    emit_suite(node.body());
    loop_has_else_ = outer_loop_has_else;
    --loop_depth_;
    write_line("}");
    if (has_else) {
        write_line("if (!" + flag + ") {");
        emit_suite(node.orelse());
        write_line("}");
        --indent_;
        write_line("}");
    }
    // Restored on EVERY path, not only the has_else one -- a loop with no
    // else is the common case, and leaving the flag set there would refuse
    // every `def` after the first module-level loop in the file.
    in_conditional_block_ = outer_conditional;
}

// The flag is set before the break so the enclosing else is skipped. When the
// innermost loop has no else the flag was never declared, and setting it
// would not compile -- so loop_depth_ alone is not enough to decide this, and
// the emitter tracks whether the current loop declared one.
void Emitter::visit(const ast::Break& node) {
    (void)node;
    if (loop_has_else_) {
        write_line("_cy_broke_" + std::to_string(loop_depth_ - 1) + " = true; break;");
        return;
    }
    write_line("break;");
}

void Emitter::visit(const ast::Continue& node) {
    (void)node;
    write_line("continue;");
}

void Emitter::visit(const ast::Return& node) {
    if (!node.has_value()) {
        // A bare `return` in a `-> None` function. The C++ return type is
        // py::none_t, which has no implicit construction from nothing, so
        // the value is spelled explicitly.
        write_line("return py::none;");
        return;
    }
    write_indent();
    write("return ");
    // CRITICAL FIX (post-review round 1): widen against the enclosing
    // function's declared return type -- `def f(x: bool) -> int: return x`
    // is mypy-clean (bool <: int) but `return cy_x;` alone would hand back a
    // bare py::bool_ from a function declared to return py::int_, which does
    // not compile.
    emit_value_widened(node.value(), return_type_);
    write(";\n");
}

// Writes "<return type> <mangled name>(<params>)" with no trailing ';' or
// '{'. Shared by emit_module's forward-declaration pass (Task 8) and this
// file's own visit(FunctionDef) below, so a forward declaration can never
// disagree with its definition -- see this function's own declaration in
// emitter.h for why that matters.
//
// Deliberately does NOT check at_module_level_ / "a nested function
// definition": that refusal is about WHERE a FunctionDef sits in the tree,
// which only visit(FunctionDef) (and never a forward-declaration pass, which
// only ever walks top-level statements) can observe.
bool Emitter::write_signature(const ast::FunctionDef& node) {
    if (!is_manglable_identifier(node.name())) {
        refuse(node, "a non-ASCII identifier");
        return false;
    }
    if (!node.has_return_annotation()) {
        refuse(node, "a function with no return annotation");
        return false;
    }

    // Resolved as a Type, not just its cpp_type_name spelling, because
    // visit(FunctionDef) below re-derives it from this same annotation to
    // seed return_type_ for the body walk -- see that call site's own
    // comment for why re-resolving rather than threading it out is safe and
    // cheap.
    const semantic::Type declared_return_type = resolve_annotation_type(node.return_annotation());
    const std::optional<std::string> return_type = cpp_type_name(declared_return_type);
    if (!return_type.has_value()) {
        refuse(node, "an unsupported return type");
        return false;
    }

    write_indent();
    write(*return_type);
    write(" ");
    write(mangle(node.name()));
    write("(");
    bool first = true;
    for (const ast::Parameter& parameter : node.params()) {
        if (parameter.annotation == nullptr) {
            refuse(node, "a parameter with no annotation");
            return false;
        }
        if (parameter.default_value != nullptr) {
            refuse(node, "a parameter with a default value");
            return false;
        }
        if (!is_manglable_identifier(parameter.name)) {
            refuse(node, "a non-ASCII identifier");
            return false;
        }
        const semantic::Type parameter_declared_type = resolve_annotation_type(*parameter.annotation);
        const std::optional<std::string> parameter_type = cpp_type_name(parameter_declared_type);
        if (!parameter_type.has_value()) {
            refuse(node, "an unsupported parameter type");
            return false;
        }
        if (!first) {
            write(", ");
        }
        first = false;
        write(*parameter_type);
        write(" ");
        write(mangle(parameter.name));
    }
    write(")");
    return true;
}

void Emitter::visit(const ast::FunctionDef& node) {
    // IMPORTANT FIX (post-review round 1): standard C++ has no nested
    // function definitions at all -- not even as a Clang extension -- while
    // Python's are fully supported upstream (TypeChecker walks them, and a
    // nested def can read/write an enclosing local via a closure). A `def`
    // reached while already inside another function's body must be refused
    // by name rather than emitting invalid syntax with no diagnostic at all.
    // Checked first, before write_signature: none of write_signature's own
    // checks are meaningful for a construct this slice cannot represent
    // regardless of how well-formed it is.
    if (!at_module_level_) {
        refuse(node, "a nested function definition");
        return;
    }
    // FINAL-REVIEW IMPORTANT 4: at_module_level_ stays TRUE inside a
    // module-level `if`/`while` body -- that body is emitted into main(), so
    // the flag is telling the truth about the SCOPE and simply cannot answer
    // "is this statement inside a block". C++ has no function definition
    // inside a block at all, and the module's forward-declaration pass walks
    // only top-level statements, so `if True:` + a `def` emitted BOTH
    // `function definition is not allowed here` and `use of undeclared
    // identifier`. A conditional `def` is genuinely outside this slice, so it
    // is refused by name -- the in-slice answer.
    if (in_conditional_block_) {
        refuse(node, "a conditional function definition");
        return;
    }
    if (!write_signature(node)) {
        return;
    }
    write(" {\n");

    // TASK 8: write_signature already validated that the return annotation
    // and every parameter annotation resolve to a supported C++ type; this
    // re-resolves the same annotations to get the Type VALUES themselves
    // (not just their cpp_type_name spelling) to seed return_type_ and
    // function_declared_ below. Resolving an annotation Expr is a pure
    // computation with no side effects of its own (see
    // resolve_annotation_type's own comment: a throwaway resolver over a
    // scratch, discarded sink), so doing it twice costs a redundant call and
    // nothing else -- the alternative, threading the Types back out of
    // write_signature, would couple that function's signature to this
    // caller's own bookkeeping needs, exactly what factoring it out was
    // meant to avoid.
    const semantic::Type declared_return_type = resolve_annotation_type(node.return_annotation());
    std::vector<semantic::Type> parameter_types;
    parameter_types.reserve(node.params().size());
    for (const ast::Parameter& parameter : node.params()) {
        parameter_types.push_back(resolve_annotation_type(*parameter.annotation));
    }

    // A nested function would need its own declared-name map, return type,
    // and module-level flag saved and restored; this slice refuses nested
    // defs outright (above), so these are simply swapped/replaced for the
    // body and restored after -- there is never a second frame live at once,
    // but the save/restore is written as though there could be, matching
    // at_module_level_'s own convention, so a future relaxation of the
    // nested-def refusal does not have to rediscover this.
    const bool outer_module_level = at_module_level_;
    const bool outer_conditional = in_conditional_block_;
    std::map<std::string, semantic::Type> outer_declared;
    outer_declared.swap(function_declared_);
    const semantic::Type outer_return_type = return_type_;
    return_type_ = declared_return_type;
    at_module_level_ = false;
    in_conditional_block_ = false;
    for (std::size_t i = 0; i < node.params().size(); ++i) {
        function_declared_.emplace(mangle(node.params()[i].name), parameter_types[i]);
    }

    // FINAL-REVIEW CRITICAL 3: the body's own declaration prologue. Every
    // local this function assigns ANYWHERE -- including inside an `if`/
    // `while` -- is declared here, at FUNCTION scope, which is the scope
    // Python itself gives it. Parameters are excluded: the signature already
    // declared them.
    std::vector<ScopeVariable> locals;
    if (!collect_scope_variables(node.body(), function_declared_, locals)) {
        at_module_level_ = outer_module_level;
        in_conditional_block_ = outer_conditional;
        return_type_ = outer_return_type;
        function_declared_.swap(outer_declared);
        return;
    }
    // The definite-assignment check runs BEFORE any of the body is emitted,
    // seeded with the parameter names (bound on entry, so `x = x + 1` on a
    // parameter is not a use-before-assignment).
    std::set<std::string> local_names;
    for (const ScopeVariable& local : locals) {
        local_names.insert(local.identifier);
    }
    std::set<std::string> parameter_names;
    for (const ast::Parameter& parameter : node.params()) {
        parameter_names.insert(parameter.name);
    }
    std::set<std::string> bound = parameter_names;
    check_definite_assignment(node.body(), local_names, bound, kScopeUnboundPhrase);

    // POST-WAVE CRITICAL: the same check carried across the FUNCTION BOUNDARY,
    // which is what the wave that added check_definite_assignment left out. A
    // module-level global whose only assignment sits in a block that may not
    // run is declared at C++ file scope regardless, so it DEFAULT-CONSTRUCTS
    // and a function reading it silently yields 0/""/false where CPython
    // raises NameError -- a program both oracles' union rule REJECTS, compiled
    // into one that runs and prints the wrong thing. Refused instead.
    //
    // A second walk rather than folding module_unbound_ into `local_names`
    // above, for two reasons: the wording differs (a module-level cause is not
    // something the function itself can fix), and SHADOWING then falls out
    // exactly right -- a name this function binds itself is already in
    // local_names, and a parameter of that name is in parameter_names, so
    // subtracting both leaves only the names whose read really does reach the
    // module-level global. `bound` for this walk is therefore empty by
    // construction: nothing left in the set is ever assigned in this body.
    if (!failed_ && !module_unbound_.empty()) {
        std::set<std::string> reaches_global;
        for (const std::string& name : module_unbound_) {
            if (local_names.count(name) == 0 && parameter_names.count(name) == 0) {
                reaches_global.insert(name);
            }
        }
        std::set<std::string> nothing_bound;
        check_definite_assignment(node.body(), reaches_global, nothing_bound,
                                  kModuleUnboundPhrase);
    }
    if (failed_) {
        at_module_level_ = outer_module_level;
        in_conditional_block_ = outer_conditional;
        return_type_ = outer_return_type;
        function_declared_.swap(outer_declared);
        return;
    }
    ++indent_;
    for (const ScopeVariable& local : locals) {
        write_line(*cpp_type_name(local.type) + " " + local.mangled + ";");
        function_declared_.emplace(local.mangled, local.type);
    }
    --indent_;

    emit_suite(node.body());
    // CRITICAL FIX (post-review round 2): a `-> None` function is the ONE
    // return type mypy never requires an explicit return on every path for
    // (TypeChecker's own end-of-FunctionDef check, further up this file's
    // history, skips its missing-return diagnostic exactly when
    // `return_type.kind == TypeKind::NoneType` -- "nothing to return, so
    // fall-through is fine"). Every OTHER resolvable return kind is
    // guaranteed a return on every path by that same check, and an
    // UNRESOLVABLE one (Unknown) always carries a diagnostic of its own,
    // which keeps such a module out of Fixture::build and the real pipeline
    // alike -- see this round's report for the full argument. So a
    // `py::none_t`-returning C++ function is the only shape whose body can
    // legitimately fall off the end with nothing emitted for it, and doing
    // so is not merely imprecise: falling off the end of a non-void C++
    // function is undefined behaviour, and measured on this project's own
    // clang++/-O0 it is a guaranteed `ud2` trap -- SIGILL at every call, for
    // the ordinary common case of a `-> None` function whose last statement
    // is a `print`, a bodyless `if`, a loop, or anything else that isn't a
    // bare `return`. Appending one unconditionally (rather than tracking
    // reachability to skip it when the body already ends in a return) trades
    // a harmless unreachable statement -- legal C++, no diagnostic -- against
    // a reachability model this stage does not otherwise need at all.
    if (declared_return_type.kind == semantic::TypeKind::NoneType) {
        ++indent_;
        write_line("return py::none;");
        --indent_;
    }
    at_module_level_ = outer_module_level;
    in_conditional_block_ = outer_conditional;
    return_type_ = outer_return_type;
    function_declared_.swap(outer_declared);

    write_line("}");
}

void Emitter::visit(const ast::For& node) { refuse(node, "a for loop"); }
void Emitter::visit(const ast::ClassDef& node) { refuse(node, "a class definition"); }

// visit(Module) and emit_module now live in emitter_module.cpp (Task 8).

} // namespace cythonpp::domain::codegen
