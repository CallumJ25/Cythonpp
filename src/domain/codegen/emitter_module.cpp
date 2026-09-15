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

// TASK 8, Step 3: walks module.body() for Assign/AnnAssign whose target is a
// plain Name, and for each name's FIRST such occurrence, writes its file-
// scope C++ declaration (e.g. "py::int_ cy_total;") and records the type in
// module_declared_ -- the record emit_assignment consults (Task 8 fix) so a
// LATER module-level reassignment widens against the DECLARED type rather
// than its own value's type, exactly as function_declared_ already does for
// a function-local variable.
//
// A target that is not a plain Name, or a name that is not manglable, is
// silently skipped here rather than refused: the corresponding statement is
// visited normally in main() (Step 6) through the ordinary
// visit(Assign)/visit(AnnAssign) path, which already refuses both cases with
// the correctly-worded diagnostic. Refusing here too would just be the same
// diagnostic reported twice.
void Emitter::emit_module_variable_declarations(const ast::Module& module) {
    for (const ast::StmtPtr& stmt : module.body()) {
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
            continue;
        }
        const std::string mangled = mangle(name->identifier());
        if (module_declared_.find(mangled) != module_declared_.end()) {
            continue; // Not the first occurrence; already declared.
        }

        // A bare `x: int` has no value at all -- resolve_assignment_type's
        // fallback chain needs a value Expr for its last link, so that link
        // is skipped entirely and the annotation (always present on an
        // AnnAssign) is used directly.
        const semantic::Type* type =
            value != nullptr ? declared_type_for_assignment(*target, *value, declared_ptr)
                             : declared_ptr;
        if (type == nullptr) {
            refuse(*target, "an assignment whose type is unknown");
            continue;
        }
        const std::optional<std::string> cpp = cpp_type_name(*type);
        if (!cpp.has_value()) {
            refuse(*target, "an assignment of an unsupported type");
            continue;
        }

        write(*cpp);
        write(" ");
        write(mangled);
        write(";\n");
        module_declared_.emplace(mangled, *type);
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
    indent_ = 0;
    function_declared_.clear();
    module_declared_.clear();

    // Step 2: the runtime include.
    write("#include \"cythonpp/cythonpp.h\"\n\n");

    // Step 3: file-scope declarations for every module-level variable.
    emit_module_variable_declarations(module);
    if (failed_) {
        return std::nullopt;
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
