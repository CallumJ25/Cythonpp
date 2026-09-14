// TEMPORARY SCAFFOLDING (Task 5). This file exists only so cythonpp_tests
// links: emitter.h declares every statement visit() plus emit_module,
// visit(Module), emit_suite and write_indent, but Task 5 implements only the
// expression side. Every body here is a placeholder refusal or a trivial
// pass-through -- Task 7 replaces the statement visits and Task 8 replaces
// the module/suite machinery wholesale.
#include "domain/codegen/emitter.h"

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
#include "domain/ast/pass.h"
#include "domain/ast/return.h"
#include "domain/ast/while.h"

namespace cythonpp::domain::codegen {

void Emitter::visit(const ast::AnnAssign& node) { refuse(node, "an annotated assignment"); }
void Emitter::visit(const ast::Assign& node) { refuse(node, "an assignment"); }
void Emitter::visit(const ast::Break& node) { refuse(node, "a break statement"); }
void Emitter::visit(const ast::ClassDef& node) { refuse(node, "a class definition"); }
void Emitter::visit(const ast::Continue& node) { refuse(node, "a continue statement"); }
void Emitter::visit(const ast::ExprStmt& node) { refuse(node, "an expression statement"); }
void Emitter::visit(const ast::For& node) { refuse(node, "a for loop"); }
void Emitter::visit(const ast::FunctionDef& node) { refuse(node, "a function definition"); }
void Emitter::visit(const ast::If& node) { refuse(node, "an if statement"); }
void Emitter::visit(const ast::Pass& node) { refuse(node, "a pass statement"); }
void Emitter::visit(const ast::Return& node) { refuse(node, "a return statement"); }
void Emitter::visit(const ast::While& node) { refuse(node, "a while loop"); }

void Emitter::write_indent() {
    // Trivial for now: no statement emission exists yet to indent.
}

void Emitter::emit_suite(const std::vector<ast::StmtPtr>& body) {
    for (const ast::StmtPtr& stmt : body) {
        stmt->accept(*this);
    }
}

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
