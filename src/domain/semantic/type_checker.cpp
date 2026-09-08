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

// RAII guard for in_class_body_: sets it to `new_value`, restores whatever
// it was on destruction. Used both ways round -- visit(ClassDef) sets it
// TRUE around walking a class's own body, visit(FunctionDef) sets it FALSE
// around walking a def's own body (a method's nested def is not itself a
// method) -- so one guard covers both, and neither an early report-and-
// return nor an exception between construction and the matching restore can
// leave the flag stuck.
class ClassBodyGuard {
public:
    ClassBodyGuard(bool& flag, bool new_value) : flag_(flag), previous_(flag) {
        flag_ = new_value;
    }
    ~ClassBodyGuard() { flag_ = previous_; }

    ClassBodyGuard(const ClassBodyGuard&) = delete;
    ClassBodyGuard& operator=(const ClassBodyGuard&) = delete;

private:
    bool& flag_;
    bool previous_;
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
            // Cached BEFORE the Binding below moves from it, so Task 18's
            // visit(FunctionDef) can reuse this exact resolution rather than
            // calling AnnotationResolver a second time on the same
            // annotations (see top_level_signatures_'s own comment).
            const Type signature_type = Type::callable(params, return_type);
            top_level_signatures_.emplace(function_def, signature_type);
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
            const Binding signature{signature_type, function_def->span().start_line,
                                    /*annotated=*/true};
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

void TypeChecker::pre_bind_function_body(const std::vector<ast::StmtPtr>& body) {
    for (const ast::StmtPtr& statement : body) {
        if (const auto* assign = dynamic_cast<const ast::Assign*>(statement.get())) {
            pre_bind_target(assign->target(), assign->span().start_line);
        } else if (const auto* ann_assign = dynamic_cast<const ast::AnnAssign*>(statement.get())) {
            if (const auto* target_name = dynamic_cast<const ast::Name*>(&ann_assign->target())) {
                if (!scopes_.bound_in_current_scope(target_name->identifier())) {
                    scopes_.bind(target_name->identifier(),
                                Binding{Type::unknown(), ann_assign->span().start_line,
                                        /*annotated=*/false});
                }
            }
        } else if (const auto* nested_def = dynamic_cast<const ast::FunctionDef*>(statement.get())) {
            if (!scopes_.bound_in_current_scope(nested_def->name())) {
                scopes_.bind(nested_def->name(),
                            Binding{Type::unknown(), nested_def->span().start_line,
                                    /*annotated=*/false});
            }
        }
        // A ClassDef nested directly in a function body is out of this
        // task's tested scope; it binds nothing into ScopeStack anywhere
        // else either (see class_lookup/ClassTable), so there is nothing to
        // placeholder-bind for one here.
    }
}

TypeChecker::AnnotationBinding TypeChecker::bind_annotation(const ast::Name& target,
                                                            const ast::Expr& annotation, int line) {
    AnnotationResolver resolver(classes_, sink_);
    Type type = resolver.resolve(annotation);
    AnnotationBinding info{type, false};
    if (scopes_.bound_in_current_scope(target.identifier())) {
        const Resolution existing = scopes_.resolve(target.identifier());
        if (existing.binding->declared_line == line) {
            // Task 18: this exact statement's own still-unfilled placeholder
            // from pre_bind_function_body (a nested AnnAssign inside a
            // function body, placeholder-bound so an earlier same-scope
            // read reports "used before definition" rather than "not
            // defined") -- fill it in rather than reporting a
            // self-redefinition. Unreachable for a module-level AnnAssign:
            // nothing placeholder-binds one of those (Phase 2's
            // collect_signatures binds the REAL type ahead of time instead),
            // so this branch is new surface area with no existing caller to
            // disturb.
            scopes_.rebind(target.identifier(), Binding{type, line, /*annotated=*/true});
            return info;
        }
        report(target, "TypeError",
              "name \"" + target.identifier() + "\" already defined on line " +
                  std::to_string(existing.binding->declared_line));
        info.redefinition = true;
        return info;
    }
    scopes_.bind(target.identifier(), Binding{type, line, /*annotated=*/true});
    return info;
}

void TypeChecker::visit(const ast::Assign& node) {
    const int line = node.span().start_line;
    typer_.set_statement_line(line);
    assign_to(node.target(), node.value(), line);
}

bool TypeChecker::is_unfilled_placeholder(const Binding& binding, int line) {
    return binding.declared_line == line && !binding.order_exempt;
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
            if (!is_unfilled_placeholder(*existing.binding, line)) {
                // A genuine prior binding (not this exact statement's own
                // still-unfilled placeholder, and not a same-line parameter
                // -- see is_unfilled_placeholder) -- use its type as
                // bidirectional context.
                expected = existing.binding->type;
            }
        }
        const Type value_type = typer_.type_of(value, expected);

        const bool is_new_definition = !scopes_.bound_in_current_scope(name->identifier()) ||
                                       is_unfilled_placeholder(
                                           *scopes_.resolve(name->identifier()).binding, line);
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
    if (is_unfilled_placeholder(*existing.binding, line)) {
        // This statement owns a still-unfilled placeholder from
        // pre_bind_assignment_targets (or is re-visiting its own earlier
        // tuple element within the same statement) -- this IS the first
        // real assignment, so fill it in rather than compare against the
        // Unknown placeholder.
        scopes_.rebind(target.identifier(), Binding{value_type, line, /*annotated=*/false});
        return;
    }
    // A genuine reassignment (including a one-line def's parameter, whose
    // declared_line equals this very statement's line but which is
    // order_exempt -- see is_unfilled_placeholder -- so it never takes the
    // branch above): the FIRST assignment's inferred type is sticky, so this
    // is a compatibility check only, never a rebind. This is what makes
    // `def f(x: int) -> None: x = "s"` a reported incompatible assignment
    // instead of a silent rebind that discards the parameter's annotation
    // (Task 18 fix round 1, Finding 1's second symptom).
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
    // is_method is captured BEFORE in_class_body_guard resets the flag, so
    // it doubles as "the value to restore when this FunctionDef is done" --
    // ClassBodyGuard's own previous_ field mirrors this exact trick. A
    // method's own nested def is not itself a method (in_class_body_ is
    // false for the whole of this function's body), which is why the reset
    // happens unconditionally rather than only when is_method is true.
    const bool is_method = in_class_body_;
    ClassBodyGuard in_class_body_guard(in_class_body_, false);

    const int def_line = node.span().start_line;
    const std::vector<ast::Parameter>& params = node.params();

    if (is_method && params.empty()) {
        // Verified against mypy 1.18.1: "Method must have at least one
        // argument. Did you forget the "self" argument?", reported ONCE at
        // the definition (mypy itself repeats it at every call site; we do
        // not). There is no self to exempt, so the "missing an annotation"
        // completeness check makes no sense here and is skipped -- but the
        // RETURN annotation, if present, is still a real expression naming a
        // real (possibly bogus) type, and mypy still reports it. Task 18 fix
        // round 1, Finding 5: this was skipped entirely before, so
        // `def m() -> Bogus:` inside a class silently swallowed the bad
        // annotation. Resolved for its diagnostic side effect only -- the
        // result feeds nothing, since the function's OWN diagnostic above is
        // already the only thing reported for its (missing) signature.
        if (node.has_return_annotation()) {
            AnnotationResolver resolver(classes_, sink_);
            resolver.resolve(node.return_annotation());
        }
        report(node, "TypeError", "method must have at least one argument");
        FunctionScopeGuard guard(scopes_);
        pre_bind_function_body(node.body());
        for (const ast::StmtPtr& statement : node.body()) {
            statement->accept(*this);
        }
        return;
    }

    // Every parameter's type, and whether it counts toward "missing an
    // annotation" -- self (a method's own first parameter) is exempt, per
    // mypy's disallow-untyped-defs. A top-level FunctionDef was already
    // resolved once by collect_signatures's Phase 2 (see
    // top_level_signatures_'s own comment); reusing that here is what keeps
    // AnnotationResolver from running -- and potentially double-reporting a
    // bad annotation -- a second time on the exact same annotation
    // expressions.
    std::vector<Type> param_types;
    Type return_type;
    bool any_param_missing = false;
    bool any_param_annotated = false;

    const auto cached = top_level_signatures_.find(&node);
    if (cached != top_level_signatures_.end() && !cached->second.args.empty()) {
        // Task 18 fix round 1, Finding 6: `args.end() - 1`/`args.back()` are
        // safe TODAY -- Type::callable (collect_signatures's own caller)
        // always pushes the return, so a cached entry's args is never empty
        // -- but this cache is populated by a DIFFERENT function than the
        // one reading it, so nothing here proves that invariant holds by
        // construction the way type_of_positional_call's identical guard
        // (expression_typer_calls.cpp) does for a Callable built through the
        // exact same Type::callable call. Guarded rather than trusted, same
        // rationale as that guard's own comment.
        const Type& signature = cached->second;
        param_types.assign(signature.args.begin(), signature.args.end() - 1);
        return_type = signature.args.back();
        for (const ast::Parameter& parameter : params) {
            if (parameter.annotation != nullptr) {
                any_param_annotated = true;
            } else {
                // A top-level def is never a method, so no index-0 self
                // exemption applies here.
                any_param_missing = true;
            }
        }
    } else {
        AnnotationResolver resolver(classes_, sink_);
        param_types.reserve(params.size());
        for (std::size_t i = 0; i < params.size(); ++i) {
            const ast::Parameter& parameter = params[i];
            const bool is_self_param = is_method && i == 0;
            if (parameter.annotation != nullptr) {
                param_types.push_back(resolver.resolve(*parameter.annotation));
                any_param_annotated = true;
            } else if (is_self_param) {
                param_types.push_back(Type::unknown());
            } else {
                param_types.push_back(Type::unknown());
                any_param_missing = true;
            }
        }
        return_type = node.has_return_annotation() ? resolver.resolve(node.return_annotation())
                                                    : Type::unknown();
    }

    // Verified against mypy 1.18.1, and contradicting Spec 5a: __init__ does
    // NOT need "-> None" when at least one parameter carries an explicit
    // annotation (self included, on the rare def that annotates it) -- only
    // a FULLY unannotated __init__ trips disallow-untyped-defs. Requiring
    // "-> None" unconditionally is a false TypeError on a mypy-clean
    // program.
    const bool init_carveout = is_method && node.name() == "__init__" && any_param_annotated;
    const bool return_missing = !node.has_return_annotation() && !init_carveout;
    if (any_param_missing || return_missing) {
        report(node, "TypeError", "function is missing a type annotation");
    }

    // A wrong-typed default is reported at the `def` line (mypy: code
    // `assignment`, not `arg-type`), each mismatch its own diagnostic like
    // every other N-bad-items rule in this checker. Defaults are typed in
    // the ENCLOSING scope, matching Python's own evaluate-at-def-time
    // semantics -- a default cannot see another parameter of the same def,
    // so this runs entirely BEFORE the Function scope below is pushed.
    typer_.set_statement_line(def_line);
    for (std::size_t i = 0; i < params.size(); ++i) {
        const ast::Parameter& parameter = params[i];
        if (parameter.default_value == nullptr) {
            continue;
        }
        const Type default_type = typer_.type_of(*parameter.default_value, param_types[i]);
        if (default_type.kind != TypeKind::Unknown && param_types[i].kind != TypeKind::Unknown &&
            !is_subtype(default_type, param_types[i], &classes_)) {
            report(node, "TypeError",
                  "incompatible default for argument \"" + parameter.name +
                      "\" (default has type \"" + type_name(default_type) +
                      "\", argument has type \"" + type_name(param_types[i]) + "\")");
        }
    }

    // The function's own name is bound BEFORE its body is checked, so
    // direct recursion works. A top-level def is already bound by Phase 2;
    // a method is NEVER bound into ScopeStack (ClassTable, Task 19, is the
    // sole source of truth there, exactly like a class's own name -- see
    // the class-level comment). What remains is a NESTED (non-method,
    // non-top-level) def: it is bound into the CURRENT (enclosing) scope,
    // at its own lexical position, no hoisting -- pre_bind_function_body
    // already placed an Unknown placeholder for it (from the ENCLOSING
    // function's own pre-bind pass, run before ITS body was walked
    // statement by statement), so an earlier same-scope call already
    // reported "used before definition" if it read this def too soon; this
    // is that placeholder's one real fill-in, mirroring assign_name's own
    // "still-unfilled placeholder" pattern.
    if (!is_method && scopes_.current_kind() == ScopeKind::Function) {
        const Type signature_type = Type::callable(param_types, return_type);
        const Binding signature{signature_type, def_line, /*annotated=*/true};
        if (scopes_.bound_in_current_scope(node.name())) {
            const Resolution existing = scopes_.resolve(node.name());
            if (existing.binding->declared_line == def_line) {
                scopes_.rebind(node.name(), signature);
            } else {
                report(node, "TypeError",
                      "name \"" + node.name() + "\" already defined on line " +
                          std::to_string(existing.binding->declared_line));
            }
        } else {
            scopes_.bind(node.name(), signature);
        }
    }

    // Parameters bind into the NEW Function scope with the `def` line, per
    // the brief -- a parameter's annotation is a declaration for the whole
    // body (`def f(x: int)` then `x = "s"` inside is a TypeError, checked
    // via assign_to/assign_name exactly like any other reassignment).
    // order_exempt=true (Task 18 fix round 1, Finding 1, CRITICAL): a
    // parameter is bound before its body runs, so it can NEVER genuinely be
    // used-before-definition inside that same body -- see Binding::
    // order_exempt's own comment for why the ordinary `>=` ordering check
    // would otherwise misfire on a one-line suite, and why
    // is_unfilled_placeholder needs this same flag to avoid mistaking a
    // same-line parameter for its own placeholder-fill case.
    FunctionScopeGuard guard(scopes_);
    for (std::size_t i = 0; i < params.size(); ++i) {
        const ast::Parameter& parameter = params[i];
        // Task 18 fix round 1, Finding 4: the bool `bind` returns MUST be
        // checked, exactly as Finding 2 of the PRIOR fix round already
        // established for collect_signatures's own def/def-collision bind
        // call -- otherwise `def f(x: int, x: str) -> None` silently binds
        // `x` once (keeping only the FIRST parameter's type) instead of
        // reporting the duplicate. Verified against mypy 1.18.1: `Duplicate
        // argument "x" in function definition`.
        if (!scopes_.bind(parameter.name, Binding{param_types[i], def_line,
                                                  /*annotated=*/parameter.annotation != nullptr,
                                                  /*order_exempt=*/true})) {
            report(node, "TypeError",
                  "duplicate argument \"" + parameter.name + "\" in function definition");
        }
    }
    // A nested `def` gets NO collect pass (verified: calling a nested
    // function defined LATER in the same body is used-before-def) -- this
    // pre-bind pass only places PLACEHOLDERS (Unknown) so the ordering
    // check can tell "used before definition" apart from "not defined"; see
    // pre_bind_function_body's own comment.
    pre_bind_function_body(node.body());
    for (const ast::StmtPtr& statement : node.body()) {
        statement->accept(*this);
    }
}

void TypeChecker::visit(const ast::ClassDef& node) {
    // ONLY tracks whether a FunctionDef sits directly in a class body, for
    // self-exemption and the __init__ carve-out above -- see the class-level
    // comment for why this is a plain bool rather than a ScopeKind::Class
    // push. Delegating to the base implementation reuses the exact
    // bases-then-body walk RecursiveVisitor::visit(ClassDef) already
    // performs, unchanged, so a class body statement is still checked in
    // whatever scope was already current (Module, until Task 19 pushes a
    // real Class scope) -- this override changes NO existing behaviour
    // besides setting/restoring the flag.
    ClassBodyGuard in_class_body_guard(in_class_body_, true);
    ast::RecursiveVisitor::visit(node);
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
