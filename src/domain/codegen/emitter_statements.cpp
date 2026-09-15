#include "domain/codegen/emitter.h"

#include <cstddef>
#include <map>
#include <string>
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
#include "domain/ast/return.h"
#include "domain/ast/while.h"
#include "domain/codegen/cpp_type_name.h"
#include "domain/codegen/name_mangler.h"
#include "domain/diagnostics/diagnostic_sink.h"
#include "domain/semantic/annotation_resolver.h"
#include "domain/semantic/class_lookup.h"
#include "domain/semantic/type.h"
#include "domain/semantic/type_compatibility.h"

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
// mapping for TypeKind::Class either way, so both answers are refused by
// cpp_type_name_of_annotation's caller identically. A class annotation is
// outside this slice regardless of which ClassLookup answered the question.
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
// for "this annotation's type is unsupported" is the nullopt
// cpp_type_name_of_annotation returns, and every caller turns that into its
// own correctly-worded refuse() against the REAL sink (e.g. "an unsupported
// parameter type"), so the user still gets exactly one diagnostic, just from
// the emitter's own reporting path rather than this scratch resolution.
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

std::optional<std::string> Emitter::cpp_type_name_of_annotation(const ast::Expr& annotation) const {
    return cpp_type_name(resolve_annotation_type(annotation));
}

void Emitter::emit_value_widened(const ast::Expr& value, const semantic::Type& target) {
    const semantic::Type* value_type = type_of(value);
    const char* widen =
        value_type != nullptr ? numeric_widening_function(value_type->kind, target.kind) : nullptr;
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

// Declares on first assignment inside a function; at module level the
// declaration is in the prelude (Task 8) and this is a plain assignment.
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
    const bool declare = !at_module_level_ && existing == nullptr;
    if (declare) {
        write(*cpp);
        write(" ");
        function_declared_.emplace(mangled, *type);
    }
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
        // that corresponds; the prelude (Task 8) still declares it.
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
    // annotation directly (cpp_type_name_of_annotation's sibling, returning
    // the Type rather than its name) closes this at the source rather than
    // leaving the fallback to guess from the value.
    const semantic::Type declared_type = resolve_annotation_type(node.annotation());
    emit_assignment(node.target(), node.value(), &declared_type);
}

void Emitter::visit(const ast::If& node) {
    write_indent();
    write("if (py::truthy(");
    emit_expr(node.condition());
    write(")) {\n");
    emit_suite(node.body());
    if (node.orelse().empty()) {
        write_line("}");
        return;
    }
    write_line("} else {");
    emit_suite(node.orelse());
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
    loop_has_else_ = has_else;
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
    std::map<std::string, semantic::Type> outer_declared;
    outer_declared.swap(function_declared_);
    const semantic::Type outer_return_type = return_type_;
    return_type_ = declared_return_type;
    at_module_level_ = false;
    for (std::size_t i = 0; i < node.params().size(); ++i) {
        function_declared_.emplace(mangle(node.params()[i].name), parameter_types[i]);
    }
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
    return_type_ = outer_return_type;
    function_declared_.swap(outer_declared);

    write_line("}");
}

void Emitter::visit(const ast::For& node) { refuse(node, "a for loop"); }
void Emitter::visit(const ast::ClassDef& node) { refuse(node, "a class definition"); }

// visit(Module) and emit_module now live in emitter_module.cpp (Task 8).

} // namespace cythonpp::domain::codegen
