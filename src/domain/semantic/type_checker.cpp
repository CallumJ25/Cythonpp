#include "type_checker.h"

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "annotation_resolver.h"
#include "builtin_call_table.h"
#include "domain/ast/call.h"
#include "domain/ast/dict_expr.h"
#include "domain/ast/function_def.h"
#include "domain/ast/list_expr.h"
#include "domain/ast/parameter.h"
#include "domain/ast/source_span.h"
#include "type_compatibility.h"
#include "type_name.h"

namespace cythonpp::domain::semantic {
namespace {

// RAII guard for the Function scope TypeChecker::visit(FunctionDef) pushes
// (fix round 1, Finding 1) -- mirrors ExpressionTyper's ComprehensionScopeGuard
// (expression_typer.cpp) so a report-and-return early exit from a future
// task's fuller FunctionDef arm can never skip the matching pop().
class FunctionScopeGuard {
public:
    explicit FunctionScopeGuard(ScopeStack& scopes) : scopes_(scopes) {
        scopes_.push(ScopeKind::Function);
    }
    ~FunctionScopeGuard() { scopes_.pop(); }

    FunctionScopeGuard(const FunctionScopeGuard&) = delete;
    FunctionScopeGuard& operator=(const FunctionScopeGuard&) = delete;

private:
    ScopeStack& scopes_;
};

} // namespace

TypeChecker::TypeChecker(diagnostics::DiagnosticSink& sink)
    : sink_(sink), scopes_(), classes_(), types_(), typer_(scopes_, classes_, types_, sink_) {}

TypeMap TypeChecker::check(const ast::Module& module) {
    module.accept(*this);
    return std::move(types_);
}

void TypeChecker::visit(const ast::Module& node) {
    scan_top_level_names(node);
    collect_classes(node);
    collect_signatures(node);
    pre_bind_assignment_targets(node);
    for (const ast::StmtPtr& statement : node.body()) {
        statement->accept(*this);
    }
}

void TypeChecker::scan_top_level_names(const ast::Module& module) {
    for (const ast::StmtPtr& statement : module.body()) {
        const ast::Node* node = nullptr;
        std::string name;
        int line = 0;
        if (const auto* class_def = dynamic_cast<const ast::ClassDef*>(statement.get())) {
            node = class_def;
            name = class_def->name();
            line = class_def->span().start_line;
        } else if (const auto* function_def = dynamic_cast<const ast::FunctionDef*>(statement.get())) {
            node = function_def;
            name = function_def->name();
            line = function_def->span().start_line;
        } else {
            continue;
        }

        const auto existing = top_level_lines_.find(name);
        if (existing != top_level_lines_.end()) {
            report(*node, "TypeError",
                  "name \"" + name + "\" already defined on line " +
                      std::to_string(existing->second));
            collided_top_level_.insert(node);
        } else {
            top_level_lines_.emplace(name, line);
        }
    }
}

std::vector<std::string> TypeChecker::base_names(const std::vector<ast::ExprPtr>& bases) {
    std::vector<std::string> names;
    for (const ast::ExprPtr& base : bases) {
        if (const auto* name = dynamic_cast<const ast::Name*>(base.get())) {
            names.push_back(name->identifier());
        }
        // A non-Name base (a subscript, an attribute chain) is outside this
        // task's tested scope and is simply omitted from the recorded base
        // list; that only matters once something walks the base chain
        // looking for it.
    }
    return names;
}

void TypeChecker::collect_classes(const ast::Module& module) {
    std::vector<const ast::ClassDef*> class_defs;
    for (const ast::StmtPtr& statement : module.body()) {
        if (const auto* class_def = dynamic_cast<const ast::ClassDef*>(statement.get())) {
            class_defs.push_back(class_def);
        }
    }

    // Declare every top-level class BEFORE resolving any base, so a base
    // naming a class declared later in the same module already resolves.
    for (const ast::ClassDef* class_def : class_defs) {
        classes_.declare(class_def->name(), base_names(class_def->bases()));
    }

    for (const ast::ClassDef* class_def : class_defs) {
        for (const ast::ExprPtr& base : class_def->bases()) {
            if (const auto* name = dynamic_cast<const ast::Name*>(base.get())) {
                if (!classes_.is_class(name->identifier())) {
                    // e.g. `class C(Generic):` -- Generic cannot be imported
                    // in this subset, so this is the correct outcome for a
                    // program nobody can legally write.
                    report(*name, "NameError", "name '" + name->identifier() + "' is not defined");
                }
            }
        }
    }
}

void TypeChecker::collect_signatures(const ast::Module& module) {
    for (const ast::StmtPtr& statement : module.body()) {
        if (const auto* function_def = dynamic_cast<const ast::FunctionDef*>(statement.get())) {
            if (collided_top_level_.count(function_def) != 0) {
                continue; // scan_top_level_names already reported this.
            }
            AnnotationResolver resolver(classes_, sink_);
            std::vector<Type> params;
            params.reserve(function_def->params().size());
            for (const ast::Parameter& parameter : function_def->params()) {
                params.push_back(parameter.annotation != nullptr
                                      ? resolver.resolve(*parameter.annotation)
                                      : Type::unknown());
            }
            Type return_type = function_def->has_return_annotation()
                                    ? resolver.resolve(function_def->return_annotation())
                                    : Type::unknown();
            // Fix round 1, Finding 2: the bool `bind` returns MUST be
            // checked -- `bind` itself has no sink and never reported
            // anything on its own, contrary to what the previous comment
            // here claimed. A def/def or def/class collision never reaches
            // this line at all: scan_top_level_names already caught and
            // reported both (it compares EVERY top-level ClassDef/FunctionDef
            // name against every other), and the `continue` above already
            // skipped the losing def. The ONE collision that reaches `bind`
            // here is an AnnAssign-then-def collision -- the AnnAssign bound
            // first, earlier in THIS same ordered loop, and
            // scan_top_level_names never tracks AnnAssign names at all -- so
            // this check exists for that case specifically. The reverse,
            // def-then-AnnAssign, is instead caught by bind_annotation's own
            // bool check below, since by the time that AnnAssign runs the def
            // already occupies the name.
            const Binding signature{Type::callable(std::move(params), std::move(return_type)),
                                    function_def->span().start_line, /*annotated=*/true};
            if (!scopes_.bind(function_def->name(), signature)) {
                const Resolution existing = scopes_.resolve(function_def->name());
                report(*function_def, "TypeError",
                      "name \"" + function_def->name() + "\" already defined on line " +
                          std::to_string(existing.binding->declared_line));
            }
        } else if (const auto* ann_assign = dynamic_cast<const ast::AnnAssign*>(statement.get())) {
            if (const auto* target_name = dynamic_cast<const ast::Name*>(&ann_assign->target())) {
                module_level_annotations_[ann_assign] = bind_annotation(
                    *target_name, ann_assign->annotation(), ann_assign->span().start_line);
            }
        }
    }
}

void TypeChecker::pre_bind_assignment_targets(const ast::Module& module) {
    for (const ast::StmtPtr& statement : module.body()) {
        if (const auto* assign = dynamic_cast<const ast::Assign*>(statement.get())) {
            pre_bind_target(assign->target(), assign->span().start_line);
        }
    }
}

void TypeChecker::pre_bind_target(const ast::Expr& target, int line) {
    if (const auto* name = dynamic_cast<const ast::Name*>(&target)) {
        if (!scopes_.bound_in_current_scope(name->identifier())) {
            scopes_.bind(name->identifier(), Binding{Type::unknown(), line, /*annotated=*/false});
        }
        return;
    }
    if (const auto* tuple = dynamic_cast<const ast::TupleExpr*>(&target)) {
        for (const ast::ExprPtr& element : tuple->elements()) {
            pre_bind_target(*element, line);
        }
        return;
    }
    // A Subscript/Attribute target mutates an existing value rather than
    // binding a new name, so there is nothing to pre-bind.
}

TypeChecker::AnnotationBinding TypeChecker::bind_annotation(const ast::Name& target,
                                                            const ast::Expr& annotation, int line) {
    AnnotationResolver resolver(classes_, sink_);
    Type type = resolver.resolve(annotation);
    AnnotationBinding info{type, false};
    if (!scopes_.bind(target.identifier(), Binding{type, line, /*annotated=*/true})) {
        const Resolution existing = scopes_.resolve(target.identifier());
        report(target, "TypeError",
              "name \"" + target.identifier() + "\" already defined on line " +
                  std::to_string(existing.binding->declared_line));
        info.redefinition = true;
    }
    return info;
}

void TypeChecker::visit(const ast::Assign& node) {
    const int line = node.span().start_line;
    typer_.set_statement_line(line);
    assign_to(node.target(), node.value(), line);
}

void TypeChecker::assign_to(const ast::Expr& target, const ast::Expr& value, int line) {
    if (const auto* tuple = dynamic_cast<const ast::TupleExpr*>(&target)) {
        assign_tuple(*tuple, value, line);
        return;
    }
    if (const auto* subscript = dynamic_cast<const ast::Subscript*>(&target)) {
        assign_subscript(*subscript, value);
        return;
    }
    if (const auto* attribute = dynamic_cast<const ast::Attribute*>(&target)) {
        assign_attribute(*attribute, value);
        return;
    }
    if (const auto* name = dynamic_cast<const ast::Name*>(&target)) {
        // Evaluation order matches Python's own: the value is evaluated
        // before the target is touched, which is exactly what makes
        // `x = x + 1` (x unbound) a used-before-definition violation rather
        // than a clean self-reference.
        Type expected = Type::unknown();
        if (scopes_.bound_in_current_scope(name->identifier())) {
            const Resolution existing = scopes_.resolve(name->identifier());
            if (existing.binding->declared_line != line) {
                // A genuine prior binding (not this exact statement's own
                // still-unfilled placeholder) -- use its type as
                // bidirectional context.
                expected = existing.binding->type;
            }
        }
        const Type value_type = typer_.type_of(value, expected);

        const bool is_new_definition = !scopes_.bound_in_current_scope(name->identifier()) ||
                                       scopes_.resolve(name->identifier()).binding->declared_line ==
                                           line;
        if (is_new_definition && is_bare_empty_container(value)) {
            report(*name, "TypeError", "need type annotation for \"" + name->identifier() + "\"");
        }
        assign_name(*name, value_type, line);
        return;
    }
    // Any other target shape is outside the supported subset; still type the
    // value so the TypeMap stays complete.
    typer_.type_of(value, Type::unknown());
}

void TypeChecker::assign_name(const ast::Name& target, const Type& value_type, int line) {
    if (!scopes_.bound_in_current_scope(target.identifier())) {
        scopes_.bind(target.identifier(), Binding{value_type, line, /*annotated=*/false});
        return;
    }
    const Resolution existing = scopes_.resolve(target.identifier());
    if (existing.binding->declared_line == line) {
        // This statement owns a still-unfilled placeholder from
        // pre_bind_assignment_targets (or is re-visiting its own earlier
        // tuple element within the same statement) -- this IS the first
        // real assignment, so fill it in rather than compare against the
        // Unknown placeholder.
        scopes_.rebind(target.identifier(), Binding{value_type, line, /*annotated=*/false});
        return;
    }
    // A genuine reassignment: the FIRST assignment's inferred type is
    // sticky, so this is a compatibility check only, never a rebind.
    if (value_type.kind != TypeKind::Unknown && existing.binding->type.kind != TypeKind::Unknown &&
        !is_subtype(value_type, existing.binding->type, &classes_)) {
        report_incompatible_assignment(target, value_type, existing.binding->type, "variable");
    }
}

void TypeChecker::assign_tuple(const ast::TupleExpr& target, const ast::Expr& value, int line) {
    const Type value_type = typer_.type_of(value, Type::unknown());
    if (value_type.kind == TypeKind::Tuple && value_type.args.size() == target.elements().size()) {
        for (std::size_t index = 0; index < target.elements().size(); ++index) {
            if (const auto* name = dynamic_cast<const ast::Name*>(target.elements()[index].get())) {
                assign_name(*name, value_type.args[index], line);
            }
        }
        return;
    }
    // An arity mismatch or a non-tuple value is outside this task's tested
    // scope. Bind each simple-Name element fresh to Unknown so a later read
    // does not cascade a spurious NameError; no diagnostic of our own here,
    // since mypy's own message for this shape is not one this compiler
    // attempts to reproduce.
    for (const ast::ExprPtr& element : target.elements()) {
        if (const auto* name = dynamic_cast<const ast::Name*>(element.get())) {
            if (!scopes_.bound_in_current_scope(name->identifier())) {
                scopes_.bind(name->identifier(), Binding{Type::unknown(), line, false});
            }
        }
    }
}

void TypeChecker::assign_subscript(const ast::Subscript& target, const ast::Expr& value) {
    const Type value_type = typer_.type_of(value, Type::unknown());
    // Reuses the read-path rule table entirely: every interesting row (a
    // non-subscriptable receiver, a bad index) already lives there, and
    // reports through the exact same mechanism a `container[index]` READ
    // would. mypy routes a store like this through __setitem__ and reports a
    // verbose call-overload error; we report a plain TypeError instead -- a
    // message difference, not a verdict difference.
    const Type element_type = typer_.type_of(target, Type::unknown());
    if (value_type.kind != TypeKind::Unknown && element_type.kind != TypeKind::Unknown &&
        !is_subtype(value_type, element_type, &classes_)) {
        report_incompatible_assignment(target, value_type, element_type, "target");
    }
}

void TypeChecker::assign_attribute(const ast::Attribute& target, const ast::Expr& value) {
    const Type value_type = typer_.type_of(value, Type::unknown());
    // Reuses type_of_attribute entirely, which already reports attr-defined
    // ("\"C\" has no attribute \"x\"") for a name the class never declares --
    // the attribute set is closed at the class definition, so assigning a
    // NEW attribute from outside the class is exactly that error.
    const Type member_type = typer_.type_of(target, Type::unknown());
    if (value_type.kind != TypeKind::Unknown && member_type.kind != TypeKind::Unknown &&
        !is_subtype(value_type, member_type, &classes_)) {
        report_incompatible_assignment(target, value_type, member_type, "target");
    }
}

void TypeChecker::visit(const ast::AnnAssign& node) {
    typer_.set_statement_line(node.span().start_line);

    AnnotationBinding info;
    const auto prebound = module_level_annotations_.find(&node);
    if (prebound != module_level_annotations_.end()) {
        // Module-level: Phase 2 already resolved (and attempted to bind)
        // this exact statement's annotation. Re-resolving here would
        // double-report a bad annotation.
        info = prebound->second;
    } else if (const auto* target_name = dynamic_cast<const ast::Name*>(&node.target())) {
        // Nested (e.g. inside a class body) -- Phase 2 only scans
        // module.body() directly, so this was never pre-bound. A forward
        // reference to a class still works: Phase 1 already declared every
        // top-level class before Phase 3 (this walk) ever started.
        info = bind_annotation(*target_name, node.annotation(), node.span().start_line);
    } else {
        // A non-Name target (outside this task's tested scope): resolve the
        // annotation for `expected` only, no binding.
        AnnotationResolver resolver(classes_, sink_);
        info.type = resolver.resolve(node.annotation());
    }

    if (!node.has_value()) {
        return;
    }
    if (info.redefinition) {
        // Already reported once; mypy reports ONLY that, never an assignment
        // error alongside it. Still type the value for the TypeMap, but
        // never against the colliding annotation's type.
        typer_.type_of(node.value(), Type::unknown());
        return;
    }
    const Type value_type = typer_.type_of(node.value(), info.type);
    if (value_type.kind != TypeKind::Unknown && info.type.kind != TypeKind::Unknown &&
        !is_subtype(value_type, info.type, &classes_)) {
        report_incompatible_assignment(node, value_type, info.type, "variable");
    }
}

void TypeChecker::visit(const ast::ExprStmt& node) {
    typer_.set_statement_line(node.span().start_line);
    typer_.type_of(node.value(), Type::unknown());
}

void TypeChecker::visit(const ast::FunctionDef& node) {
    // Fix round 1, Finding 1: push a Function scope before walking the body.
    // Without this, the body is checked in whatever scope was already
    // current (Module, at top level), so ExpressionTyper's Name arm sees
    // in_own_scope == true for a global and wrongly order-checks it -- a
    // function reading a module global assigned LATER in the file (mypy-
    // clean, PEP 649) got a false "used before definition". With the scope
    // pushed, the same read resolves outward (in_own_scope == false) and is
    // exempt, per the ordering rule's own "outward reads are exempt" clause.
    //
    // Deliberately minimal: no parameter binding, no annotation resolution,
    // no return-type checking, no __init__ carve-out. Task 18 fills in the
    // rest of this arm.
    FunctionScopeGuard guard(scopes_);
    for (const ast::StmtPtr& statement : node.body()) {
        statement->accept(*this);
    }
}

bool TypeChecker::is_bare_empty_container(const ast::Expr& value) {
    if (const auto* list = dynamic_cast<const ast::ListExpr*>(&value)) {
        return list->elements().empty();
    }
    if (const auto* dict = dynamic_cast<const ast::DictExpr*>(&value)) {
        return dict->entries().empty();
    }
    if (const auto* call = dynamic_cast<const ast::Call*>(&value)) {
        if (!call->args().empty()) {
            return false;
        }
        const auto* callee = dynamic_cast<const ast::Name*>(&call->callee());
        if (callee == nullptr) {
            return false;
        }
        // Fix round 1, Finding 4: reuse builtin_call_table.h's exported
        // is_empty_display_builtin rather than a third hardcoded copy of the
        // five-name list -- expression_typer_calls.cpp already carries a
        // comment justifying its own local helper specifically "so the two
        // call sites cannot drift apart"; a third copy here would be exactly
        // the drift that comment warns against.
        return is_empty_display_builtin(callee->identifier());
    }
    // A bare `()` (TupleExpr) is deliberately NOT here: tuple[()] is a
    // complete, non-generic type needing no annotation.
    return false;
}

void TypeChecker::report(const ast::Node& at, std::string code, std::string message) {
    const ast::SourceSpan span = at.span();
    sink_.report_error(std::move(code), std::move(message), span.start_line, span.start_column);
}

void TypeChecker::report_incompatible_assignment(const ast::Node& at, const Type& value_type,
                                                 const Type& target_type,
                                                 const char* target_label) {
    report(at, "TypeError",
          "incompatible types in assignment (expression has type \"" + type_name(value_type) +
              "\", " + target_label + " has type \"" + type_name(target_type) + "\")");
}

} // namespace cythonpp::domain::semantic
