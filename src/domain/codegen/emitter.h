#ifndef CYTHONPP_DOMAIN_CODEGEN_EMITTER_H
#define CYTHONPP_DOMAIN_CODEGEN_EMITTER_H

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// stmt.h rather than a forward declaration of ast::Stmt: emit_suite's
// signature names StmtPtr, which is that header's alias, and a forward
// declaration cannot supply it.
#include "domain/ast/stmt.h"
#include "domain/ast/visitor.h"
#include "domain/diagnostics/diagnostic_sink.h"
#include "domain/semantic/type.h"
#include "domain/semantic/type_map.h"

namespace cythonpp::domain::ast {
class Expr;
class Module;
} // namespace cythonpp::domain::ast

namespace cythonpp::domain::codegen {

// Turns a type-checked ast::Module into standalone C++ source.
//
// AN ast::Visitor, NOT AN ast::RecursiveVisitor, and the choice is the
// inverse of TypeChecker's for the inverse reason. RecursiveVisitor's default
// walks an unhandled node's children anyway; for a type checker that is a
// missed rule, but for an emitter it is a SILENTLY DROPPED STATEMENT. The
// twenty-six pure-virtual visit methods force a new AST node to be considered
// at the exact spot needing attention.
//
// ONE CLASS ACROSS THREE TRANSLATION UNITS (emitter_expressions.cpp,
// emitter_statements.cpp, emitter_module.cpp), the same arrangement
// expression_typer.cpp and expression_typer_calls.cpp already use, and for
// the same reason: size. Two Visitor SUBCLASSES would mean fifty-two
// overrides for twenty-six nodes, half of them stubs with nothing honest to
// report.
//
// OPERATOR PRECEDENCE IS NOT A PROBLEM THIS CLASS HAS. Every Python operation
// emits as a runtime function CALL -- py::mul(py::add(a, b), c) -- so there is
// no precedence to reconstruct, no associativity to preserve, and no
// parenthesisation logic to get wrong. That is also why a single append-only
// buffer suffices: sub-expressions compose in source order.
//
// FAILURE IS ALL-OR-NOTHING. Any refusal sets failed_, and every public entry
// point yields nullopt when it is set, so a partially-emitted file never
// reaches disk. A half-written .cpp whose failure surfaces only when the user
// runs clang++ on it is precisely the silent miscompile this stage exists to
// prevent.
class Emitter : public ast::Visitor {
public:
    Emitter(const semantic::TypeMap& types, diagnostics::DiagnosticSink& sink);

    // The whole module as one .cpp, or nullopt if anything was refused.
    // Implemented in emitter_module.cpp (Task 8).
    std::optional<std::string> emit_module(const ast::Module& module);

    // Test-only seam: emit one expression in isolation. Production code goes
    // through emit_module. Exposed because an expression's emitted text is
    // the unit worth pinning, and reaching it through a whole module would
    // make every expression test assert on boilerplate too.
    std::optional<std::string> emit_expression_for_test(const ast::Expr& expr);

    // Test-only seam: emit one statement in isolation, the statement-side
    // counterpart of emit_expression_for_test above, for exactly the same
    // reason -- pinning one statement's emitted text without dragging in a
    // whole module's boilerplate.
    std::optional<std::string> emit_statement_for_test(const ast::Stmt& statement);

    // Expressions (emitter_expressions.cpp, Tasks 5-6).
    void visit(const ast::Attribute& node) override;
    void visit(const ast::BinOp& node) override;
    void visit(const ast::BoolOp& node) override;
    void visit(const ast::Call& node) override;
    void visit(const ast::Compare& node) override;
    void visit(const ast::Constant& node) override;
    void visit(const ast::DictExpr& node) override;
    void visit(const ast::ListComp& node) override;
    void visit(const ast::ListExpr& node) override;
    void visit(const ast::Name& node) override;
    void visit(const ast::Subscript& node) override;
    void visit(const ast::TupleExpr& node) override;
    void visit(const ast::UnaryOp& node) override;

    // Statements (emitter_statements.cpp, Task 7).
    void visit(const ast::AnnAssign& node) override;
    void visit(const ast::Assign& node) override;
    void visit(const ast::Break& node) override;
    void visit(const ast::ClassDef& node) override;
    void visit(const ast::Continue& node) override;
    void visit(const ast::ExprStmt& node) override;
    void visit(const ast::For& node) override;
    void visit(const ast::FunctionDef& node) override;
    void visit(const ast::If& node) override;
    void visit(const ast::Pass& node) override;
    void visit(const ast::Return& node) override;
    void visit(const ast::While& node) override;

    // The root (emitter_module.cpp, Task 8).
    void visit(const ast::Module& node) override;

private:
    // Appends to the buffer. The single choke point, so a future change to
    // how text accumulates has exactly one place to go.
    void write(std::string_view text);

    // Emits a sub-expression in place.
    void emit_expr(const ast::Expr& expr);

    // Reports one NotImplementedError naming `what` and marks the emission
    // failed. Takes the node for its span, so the diagnostic points at the
    // construct rather than at the file.
    void refuse(const ast::Node& node, const std::string& what);

    // The static type the checker gave this expression, or nullptr. A null
    // answer is itself a refusal condition at every call site: an expression
    // with no recorded type has no C++ type to emit.
    const semantic::Type* type_of(const ast::Expr& expr) const;

    // Task 6 helpers, declared here so Task 5's file compiles unchanged.
    void emit_binary(const ast::BinOp& node);
    void emit_power(const ast::BinOp& node);
    void emit_call(const ast::Call& node);
    void emit_operand_widened(const ast::Expr& operand, const semantic::Type& result);

    // Task 7-8 helpers.
    void emit_suite(const std::vector<ast::StmtPtr>& body);
    void write_indent();
    void write_line(std::string_view text);
    void emit_assignment(const ast::Expr& target, const ast::Expr& value,
                         const semantic::Type* declared);

    // A function/parameter/AnnAssign annotation Expr resolved to a Type via a
    // throwaway AnnotationResolver -- see emitter_statements.cpp's own
    // "HOLE (B)" comment for why a scratch ClassLookup and a discarded sink
    // are safe for the scalar slice this stage admits. A MEMBER function
    // (rather than a free function local to one translation unit) so
    // emitter_module.cpp's file-scope variable prelude can resolve a
    // module-level AnnAssign's annotation identically to emit_assignment,
    // instead of re-deriving the same throwaway-resolver dance a second time.
    semantic::Type resolve_annotation_type(const ast::Expr& annotation) const;

    // The type an assignment TARGET's declaration should carry: `declared`
    // when given (an AnnAssign's own resolved annotation), else
    // type_of(target) (always nullptr for a Name target -- TypeMap holds no
    // assignment-target entries, see type_map.h's own comment), else
    // type_of(value). Factored out of emit_assignment so emit_module's
    // module-level variable prelude computes the identical answer for the
    // identical statement rather than a second, potentially-drifting copy of
    // this fallback chain.
    const semantic::Type* declared_type_for_assignment(const ast::Expr& target,
                                                       const ast::Expr& value,
                                                       const semantic::Type* declared) const;

    // Writes a FunctionDef's return type, mangled name and parenthesised
    // parameter list -- e.g. "py::int_ cy_f(py::int_ cy_n)" -- with no
    // trailing ';' or '{'. Returns false (having already called refuse()) if
    // any part of the signature is outside this slice. Shared by the
    // forward-declaration pass and the real definition in emitter_module.cpp
    // so the two can never disagree: a forward declaration that disagrees
    // with its definition is a link error at best and a silently wrong call
    // at worst.
    bool write_signature(const ast::FunctionDef& node);

    // Step 3 of emit_module: walks module.body() for a module-level
    // Assign/AnnAssign and writes a file-scope C++ declaration for each
    // name's first occurrence, recording its type in module_declared_.
    // Implemented in emitter_module.cpp; declared here (rather than kept
    // file-local) purely because it is a member -- it has no callers outside
    // that one file.
    void emit_module_variable_declarations(const ast::Module& module);

    // Emits `value`, wrapped in the runtime's explicit py::to_int/py::to_float
    // conversion when `value`'s own static type is a PROPER subtype of
    // `target` within Python's numeric tower (bool <: int <: float) -- a real
    // subtyping relationship is_subtype enforces, so `x: float = 1` and
    // `def f(x: bool) -> int: return x` both type-check clean under mypy, yet
    // py::int_/py::bool_ have no implicit conversion to py::float_/py::int_
    // (see runtime/cythonpp/int_.h's own comment: that is deliberate, so the
    // add/sub/mul overload sets stay unambiguous). Backs every site that puts
    // a value where a DECLARED C++ type is expected rather than an inferred
    // one: emit_assignment's initializer and visit(Return)'s value.
    void emit_value_widened(const ast::Expr& value, const semantic::Type& target);

    // A function's parameter and return types (and an AnnAssign's declared
    // type) come from ANNOTATIONS, not from an expression the checker typed
    // -- TypeMap deliberately holds no annotation-subtree entries (see
    // type_map.h's own comment). This resolves one through
    // semantic::AnnotationResolver and feeds the result to cpp_type_name; see
    // emitter_statements.cpp for the class-lookup stand-in and the sink this
    // uses, and why both are safe for the scalar slice this stage admits.
    std::optional<std::string> cpp_type_name_of_annotation(const ast::Expr& annotation) const;

    // Names already declared in the CURRENT function body, mapped to the C++
    // type each was declared with. Empty at module level, where declarations
    // live in the file-scope prelude instead. A MAP rather than a set of
    // names: a REASSIGNMENT (`x = 2.5` with no annotation of its own) must
    // widen against the type the name was first declared with, not against
    // whatever this particular value's own type happens to be -- the two can
    // legitimately differ (`x: float = 1.0` then `x = 2`), and a mere set
    // cannot answer "declared as what".
    std::map<std::string, semantic::Type> function_declared_;
    bool at_module_level_ = true;

    // The module-scope counterpart of function_declared_: the declared type
    // of every module-level variable, populated once by emit_module's
    // file-scope prelude before main() is emitted. function_declared_ is
    // swapped out and empty at module level (it exists only for the
    // CURRENTLY-EMITTING function's body), so a module-level reassignment
    // (`x = 2` after `x: float = 1`) needs its OWN record to widen against --
    // without it, emit_assignment's existing-declaration lookup finds
    // nothing at module level and falls back to the value's own type,
    // producing e.g. a bare `py::int_(2)` assigned into a `py::float_`
    // global, which does not compile.
    std::map<std::string, semantic::Type> module_declared_;

    // The enclosing function's declared return type, consulted by
    // visit(Return) to widen its value exactly as emit_assignment does for an
    // initializer. Meaningless at module level (Python itself rejects a
    // `return` there, so nothing reads this while at_module_level_ is true).
    semantic::Type return_type_;

    // Whether the loop currently being emitted declared a `_cy_broke_<depth>`
    // flag, i.e. whether it has an `else` clause. Saved and restored around a
    // loop's own body exactly as at_module_level_ is around a function's,
    // because loops nest: an inner loop with no else, inside an outer one
    // that has one, must still emit a bare `break;` rather than reference a
    // flag only the OUTER loop declared.
    bool loop_has_else_ = false;

    const semantic::TypeMap& types_;
    diagnostics::DiagnosticSink& sink_;
    std::string out_;
    bool failed_ = false;
    int indent_ = 0;
    int loop_depth_ = 0;
};

} // namespace cythonpp::domain::codegen

#endif // CYTHONPP_DOMAIN_CODEGEN_EMITTER_H
