#include "domain/codegen/emitter.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "domain/ast/ann_assign.h"
#include "domain/ast/assign.h"
#include "domain/ast/function_def.h"
#include "domain/ast/module.h"
#include "domain/ast/name.h"
#include "domain/codegen/cpp_type_name.h"
#include "domain/codegen/name_mangler.h"
#include "domain/semantic/type.h"

namespace cythonpp::domain::codegen {

// TASK 8, Step 3: writes a file-scope C++ declaration (e.g.
// "py::int_ cy_total;") for every module-level variable and records its type
// in module_declared_ -- the record emit_assignment consults (Task 8 fix) so a
// LATER module-level reassignment widens against the DECLARED type rather
// than its own value's type, exactly as function_declared_ already does for
// a function-local variable.
//
// FILE SCOPE, not a local inside main(), because a `def` must be able to read
// a module-level variable and a C++ local in main() is invisible to a free
// function.
//
// FINAL-REVIEW CRITICAL 3: the collection itself now lives in the shared
// collect_scope_variables, which RECURSES into `if`/`while` suites. This
// function used to walk module.body() alone, so `if x > 0: y: int = 2`
// followed by `print(y)` emitted an assignment to a name declared nowhere at
// all. A module-level `if` body is the same Python scope as the module, so
// its assignments belong in this prelude.
void Emitter::emit_module_variable_declarations(const ast::Module& module,
                                                std::set<std::string>& identifiers) {
    std::vector<ScopeVariable> variables;
    if (!collect_scope_variables(module.body(), module_declared_, variables)) {
        return;
    }
    for (const ScopeVariable& variable : variables) {
        write(*cpp_type_name(variable.type));
        write(" ");
        write(variable.mangled);
        write(";\n");
        module_declared_.emplace(variable.mangled, variable.type);
        identifiers.insert(variable.identifier);
    }
}

// TASK 8, Step 1 of emit_module's overall assembly (this function's own two
// steps are 1-2, the caller sequences 3-7). visit(Module) itself must not be
// reachable in normal operation -- it exists only to satisfy Visitor's pure
// virtual contract.
void Emitter::visit(const ast::Module& node) { refuse(node, "a nested module"); }

std::optional<std::string> Emitter::emit_module(const ast::Module& module) {
    // Step 1: reset all per-emission state. at_module_level_ and
    // function_declared_ already default/reset correctly for a fresh
    // Emitter, but emit_module may be called more than once on the same
    // instance (emit_expression_for_test/emit_statement_for_test are its
    // siblings, each clearing their own state at entry), so every piece of
    // state this pass touches is reset explicitly rather than trusted to
    // still hold its constructed default.
    out_.clear();
    failed_ = false;
    at_module_level_ = true;
    in_conditional_block_ = false;
    indent_ = 0;
    function_declared_.clear();
    module_declared_.clear();

    // Step 2: the runtime include.
    write("#include \"cythonpp/cythonpp.h\"\n\n");

    // Step 3: file-scope declarations for every module-level variable.
    std::set<std::string> module_names;
    emit_module_variable_declarations(module, module_names);
    if (failed_) {
        return std::nullopt;
    }

    // FINAL-REVIEW CRITICAL 3, half two: the module body's own definite-
    // assignment check, the exact counterpart of the one visit(FunctionDef)
    // runs over a function body. Module-level statements execute in source
    // order inside main(); a FunctionDef's BODY does not run here at all, and
    // check_definite_assignment skips it for that reason. Nothing is bound on
    // entry, so `bound` starts empty.
    {
        std::set<std::string> bound;
        check_definite_assignment(module.body(), module_names, bound);
        if (failed_) {
            return std::nullopt;
        }
    }
    if (!module_declared_.empty()) {
        write("\n");
    }

    // Step 4: forward declarations for every module-level def, via the same
    // write_signature both this pass and the real definition (Step 5) call,
    // so the two cannot disagree.
    bool any_function = false;
    for (const ast::StmtPtr& stmt : module.body()) {
        const auto* function_def = dynamic_cast<const ast::FunctionDef*>(stmt.get());
        if (function_def == nullptr) {
            continue;
        }
        any_function = true;
        if (!write_signature(*function_def)) {
            return std::nullopt;
        }
        write(";\n");
    }
    if (any_function) {
        write("\n");
    }

    // Step 5: the definitions, through the normal visit dispatch.
    for (const ast::StmtPtr& stmt : module.body()) {
        if (dynamic_cast<const ast::FunctionDef*>(stmt.get()) == nullptr) {
            continue;
        }
        stmt->accept(*this);
        if (failed_) {
            return std::nullopt;
        }
    }

    // Step 6: main(), carrying every non-FunctionDef top-level statement.
    write("int main() {\n");
    at_module_level_ = true;
    indent_ = 1;
    for (const ast::StmtPtr& stmt : module.body()) {
        if (dynamic_cast<const ast::FunctionDef*>(stmt.get()) != nullptr) {
            continue;
        }
        stmt->accept(*this);
        if (failed_) {
            return std::nullopt;
        }
    }
    write_line("return 0;");
    indent_ = 0;
    write("}\n");

    // Step 7.
    if (failed_) {
        return std::nullopt;
    }
    return out_;
}

} // namespace cythonpp::domain::codegen
