#ifndef CYTHONPP_DOMAIN_CODEGEN_EMITTER_H
#define CYTHONPP_DOMAIN_CODEGEN_EMITTER_H

#include <optional>
#include <set>
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

    // A function's parameter and return types (and an AnnAssign's declared
    // type) come from ANNOTATIONS, not from an expression the checker typed
    // -- TypeMap deliberately holds no annotation-subtree entries (see
    // type_map.h's own comment). This resolves one through
    // semantic::AnnotationResolver and feeds the result to cpp_type_name; see
    // emitter_statements.cpp for the class-lookup stand-in and the sink this
    // uses, and why both are safe for the scalar slice this stage admits.
    std::optional<std::string> cpp_type_name_of_annotation(const ast::Expr& annotation) const;

    // Names already declared in the CURRENT function body. Empty at module
    // level, where declarations live in the file-scope prelude instead.
    std::set<std::string> function_declared_;
    bool at_module_level_ = true;

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
