#include "domain/codegen/emitter.h"

#include <string>
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
#include "domain/ast/module.h"
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
semantic::Type resolve_annotation_type(const ast::Expr& annotation) {
    NullClassLookup classes;
    diagnostics::DiagnosticSink discarded;
    semantic::AnnotationResolver resolver(classes, discarded);
    return resolver.resolve(annotation);
}

} // namespace

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
    const semantic::Type* type = declared != nullptr ? declared : type_of(target);
    if (type == nullptr) {
        type = type_of(value);
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
    const bool declare = !at_module_level_ && function_declared_.insert(mangled).second;
    if (declare) {
        write(*cpp);
        write(" ");
    }
    write(mangled);
    write(" = ");
    emit_expr(value);
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
    emit_expr(node.value());
    write(";\n");
}

void Emitter::visit(const ast::FunctionDef& node) {
    if (!is_manglable_identifier(node.name())) {
        refuse(node, "a non-ASCII identifier");
        return;
    }
    if (!node.has_return_annotation()) {
        refuse(node, "a function with no return annotation");
        return;
    }

    const std::optional<std::string> return_type = cpp_type_name_of_annotation(node.return_annotation());
    if (!return_type.has_value()) {
        refuse(node, "an unsupported return type");
        return;
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
            return;
        }
        if (parameter.default_value != nullptr) {
            refuse(node, "a parameter with a default value");
            return;
        }
        if (!is_manglable_identifier(parameter.name)) {
            refuse(node, "a non-ASCII identifier");
            return;
        }
        const std::optional<std::string> parameter_type =
            cpp_type_name_of_annotation(*parameter.annotation);
        if (!parameter_type.has_value()) {
            refuse(node, "an unsupported parameter type");
            return;
        }
        if (!first) {
            write(", ");
        }
        first = false;
        write(*parameter_type);
        write(" ");
        write(mangle(parameter.name));
    }
    write(") {\n");

    // A nested function would need its own declared-name set saved and
    // restored; this slice has none, so the sets are simply swapped for the
    // body and restored after.
    const bool outer_module_level = at_module_level_;
    std::set<std::string> outer_declared;
    outer_declared.swap(function_declared_);
    at_module_level_ = false;
    for (const ast::Parameter& parameter : node.params()) {
        function_declared_.insert(mangle(parameter.name));
    }
    emit_suite(node.body());
    at_module_level_ = outer_module_level;
    function_declared_.swap(outer_declared);

    write_line("}");
}

void Emitter::visit(const ast::For& node) { refuse(node, "a for loop"); }
void Emitter::visit(const ast::ClassDef& node) { refuse(node, "a class definition"); }

// Minimal stubs carried forward from Task 5's scaffolding. Task 8 replaces
// both with the real prelude/module-assembly logic (file-scope globals,
// runtime includes, and so on); until then this is just enough for
// emit_statement_for_test's callers (which build a Module via the real
// parser/checker pipeline but never call emit_module itself) to link, and
// for visit(Module) to exist at all -- ast::Visitor gives it no default.
void Emitter::visit(const ast::Module& node) { emit_suite(node.body()); }

std::optional<std::string> Emitter::emit_module(const ast::Module& module) {
    out_.clear();
    failed_ = false;
    module.accept(*this);
    if (failed_) {
        return std::nullopt;
    }
    return out_;
}

} // namespace cythonpp::domain::codegen
