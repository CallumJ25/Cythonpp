#include "type_checker.h"

#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "annotation_resolver.h"
#include "builtin_call_table.h"
#include "domain/ast/break.h"
#include "domain/ast/call.h"
#include "domain/ast/constant.h"
#include "domain/ast/dict_expr.h"
#include "domain/ast/function_def.h"
#include "domain/ast/list_expr.h"
#include "domain/ast/parameter.h"
#include "domain/ast/source_span.h"
#include "domain/lexer/token_type.h"
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

// RAII guard for visit(ClassDef)'s class body: pushes a REAL ScopeKind::Class
// (Task 19 -- a prior, minimal ClassDef override pushed nothing at all, just
// a bool; see type_checker.h's class-level comment for why that was enough
// before this task and is not now) and sets current_class_qualified_name_ to
// `qualified_name`, restoring both on destruction -- so nested classes
// compose correctly by simple stack discipline: Inner's own guard, built
// while Outer's is still live, saves "Outer" as `previous_` and restores it
// once Inner's body is done, with no explicit nesting-depth bookkeeping
// anywhere.
class ClassContextGuard {
public:
    ClassContextGuard(ScopeStack& scopes, std::string& current_name, std::string qualified_name)
        : scopes_(scopes), current_name_(current_name), previous_(current_name) {
        scopes_.push(ScopeKind::Class);
        current_name_ = std::move(qualified_name);
    }
    ~ClassContextGuard() {
        scopes_.pop();
        current_name_ = std::move(previous_);
    }

    ClassContextGuard(const ClassContextGuard&) = delete;
    ClassContextGuard& operator=(const ClassContextGuard&) = delete;

private:
    ScopeStack& scopes_;
    std::string& current_name_;
    std::string previous_;
};

// RAII guard for the declared return type Return's own checks (Task 20) read
// -- current_return_type_ -- saved and restored exactly like
// ClassContextGuard restores current_class_qualified_name_, so a nested def's
// own Return statements are checked against ITS OWN return type rather than
// the enclosing function's.
class ReturnContextGuard {
public:
    ReturnContextGuard(Type& current, Type new_type)
        : current_(current), previous_(current) {
        current_ = std::move(new_type);
    }
    ~ReturnContextGuard() { current_ = std::move(previous_); }

    ReturnContextGuard(const ReturnContextGuard&) = delete;
    ReturnContextGuard& operator=(const ReturnContextGuard&) = delete;

private:
    Type& current_;
    Type previous_;
};

// The `while True` half of always_returns' While arm: true only for a
// Constant whose token type is BOOL_TRUE, per the brief's own precise
// definition -- NOT any expression ExpressionTyper would type as `bool`
// (e.g. a bare `1` is truthy but not this), because always_returns is a
// SYNTACTIC approximation with no typing pass of its own.
bool is_literal_true(const ast::Expr& condition) {
    const auto* constant = dynamic_cast<const ast::Constant*>(&condition);
    return constant != nullptr && constant->type() == lexer::token_type::BOOL_TRUE;
}

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
    std::vector<const ast::ClassDef*> all_classes;
    for (const ast::StmtPtr& statement : module.body()) {
        if (const auto* class_def = dynamic_cast<const ast::ClassDef*>(statement.get())) {
            if (collided_top_level_.count(class_def) != 0) {
                // Task 19 fix: scan_top_level_names already reported this
                // ClassDef as a redefinition. Declaring it anyway used to
                // silently OVERWRITE the winning same-named class's
                // ClassTable entry (declare() has no collision detection of
                // its own), so a later use (a member lookup, a constructor
                // call) resolved against the LOSING class's bases/members --
                // collect_signatures already skips a collided FunctionDef
                // for the identical reason.
                continue;
            }
            declare_class_recursive(*class_def, "", all_classes);
        }
    }

    // THEN -- once every class at every nesting depth is declared -- validate
    // that each bare-Name base actually resolves, reporting NameError for one
    // that does not (e.g. `class C(Generic):`, since Generic cannot be
    // imported in this subset). Deferred until here (rather than folded into
    // declare_class_recursive) so a base naming a class declared LATER in the
    // same module, or in a different class's body, already resolves.
    validate_class_bases(all_classes);
}

void TypeChecker::validate_class_bases(const std::vector<const ast::ClassDef*>& all_classes) {
    for (const ast::ClassDef* class_def : all_classes) {
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

std::string TypeChecker::declare_isolated_class(const ast::ClassDef& node,
                                                const std::string& qualified_name) {
    classes_.declare(qualified_name, base_names(node.bases()));
    std::vector<const ast::ClassDef*> all_classes{&node};
    for (const ast::StmtPtr& statement : node.body()) {
        if (const auto* nested = dynamic_cast<const ast::ClassDef*>(statement.get())) {
            declare_class_recursive(*nested, qualified_name, all_classes);
        }
    }
    validate_class_bases(all_classes);
    return qualified_name;
}

void TypeChecker::declare_class_recursive(const ast::ClassDef& class_def,
                                          const std::string& qualified_prefix,
                                          std::vector<const ast::ClassDef*>& all_classes) {
    const std::string qualified_name =
        qualified_prefix.empty() ? class_def.name() : qualified_prefix + "." + class_def.name();
    classes_.declare(qualified_name, base_names(class_def.bases()));
    all_classes.push_back(&class_def);

    // A NESTED ClassDef (e.g. Inner inside Outer's body) is declared right
    // here, under ITS OWN qualified name -- "Outer.Inner" -- rather than
    // waiting for Phase 3's ordinary walk to reach it: an annotation
    // resolved in Phase 2 (`x: Outer.Inner`) runs BEFORE Phase 3 ever visits
    // Outer's ClassDef node, so without this recursion "Outer.Inner" would
    // not exist in ClassTable yet and the annotation would report a false
    // NameError.
    for (const ast::StmtPtr& statement : class_def.body()) {
        if (const auto* nested = dynamic_cast<const ast::ClassDef*>(statement.get())) {
            declare_class_recursive(*nested, qualified_name, all_classes);
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
    return bind_resolved_annotation(target, resolver.resolve(annotation), line);
}

TypeChecker::AnnotationBinding TypeChecker::bind_resolved_annotation(const ast::Name& target,
                                                                      Type type, int line) {
    AnnotationBinding info{type, false};
    if (scopes_.bound_in_current_scope(target.identifier())) {
        const Resolution existing = scopes_.resolve(target.identifier());
        if (is_unfilled_placeholder(*existing.binding, line)) {
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
            //
            // Fix round 2: routed through is_unfilled_placeholder (rather
            // than the raw declared_line == line this used before) so an
            // order_exempt PARAMETER binding is never mistaken for this
            // function's own unfilled placeholder -- a same-line annotated
            // re-assignment of a parameter (`def f(x: int) -> None:
            // x: str = "s"`) must fall through to the redefinition report
            // below, exactly like the multi-line form already does, instead
            // of silently rebinding over the parameter's real annotation.
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
        if (is_new_definition && scopes_.current_kind() == ScopeKind::Class) {
            // Fix round 1, Finding 3: a plain class-body Assign (`class D:
            // x = 5`) never declared an instance attribute at all before
            // this -- only the AnnAssign path did. Declared on the FIRST
            // real assignment only (is_new_definition, already computed
            // above for the bare-empty-container check), matching every
            // other "first assignment is sticky" rule in this checker --
            // pre_collect_class_body may already have placeholder-declared
            // this exact name as Unknown at this exact line (so a method
            // ABOVE this statement reading self.x already sees it exists).
            //
            // Fix round 2, Finding A: round 1's own fix here had NO guard at
            // all -- unconditionally overwriting classes_.declare_member,
            // even when a member ALREADY exists under a DIFFERENT line (a
            // genuine earlier self.x = ... assignment in a method ABOVE this
            // statement), silently RE-TYPING the attribute with zero
            // diagnostics. That is the exact defect round 1's own Finding 5
            // already fixed for the AnnAssign sibling and assign_attribute's
            // own self.x path; this was the one place the guard was missed.
            // Routed through the SAME member_declared_line line-equality
            // disambiguation assign_attribute already uses: a member at
            // THIS exact line is this statement's own placeholder (fill in
            // the real type -- no comparison, nothing real to compare
            // against yet); anything else is a genuine earlier declaration,
            // compared rather than silently overwritten.
            const bool already_method =
                classes_.method_type(current_class_qualified_name_, name->identifier()).has_value();
            const std::optional<int> existing_line =
                classes_.member_declared_line(current_class_qualified_name_, name->identifier());
            const bool is_brand_new = !already_method && !existing_line.has_value();
            const bool is_own_placeholder =
                !already_method && existing_line.has_value() && *existing_line == line;
            if (is_brand_new || is_own_placeholder) {
                classes_.declare_member(current_class_qualified_name_, name->identifier(), value_type,
                                        line);
            } else if (!already_method) {
                const Type existing_type =
                    *classes_.member_type(current_class_qualified_name_, name->identifier());
                if (value_type.kind != TypeKind::Unknown && existing_type.kind != TypeKind::Unknown &&
                    !is_subtype(value_type, existing_type, &classes_)) {
                    report_incompatible_assignment(*name, value_type, existing_type, "variable");
                }
            }
            // A same-name METHOD collision is left unreported here, matching
            // the AnnAssign branch's own precedent.
        }
        return;
    }
    // Any other target shape is outside the supported subset; still type the
    // value so the TypeMap stays complete.
    typer_.type_of(value, Type::unknown());
}

void TypeChecker::assign_name(const ast::Name& target, const Type& value_type, int line,
                              bool order_exempt) {
    if (!scopes_.bound_in_current_scope(target.identifier())) {
        scopes_.bind(target.identifier(),
                     Binding{value_type, line, /*annotated=*/false, order_exempt});
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
    // THE TRAP's escape hatch (Task 19): `self.x = ...` inside a method
    // declares a NEW instance attribute the first time it is seen, checked
    // BEFORE the ordinary read-then-compare path below -- which would
    // otherwise call type_of_attribute on a member that does not exist YET
    // and report a false attr-defined TypeError. Purely syntactic plus one
    // ScopeStack::resolve (never itself typed, so this check alone can never
    // report anything): the receiver must be a bare Name spelled "self" that
    // currently resolves to Class(current_class_qualified_name_) -- i.e. we
    // are really inside one of that class's own methods, not merely inside
    // some unrelated nested function that happens to have a parameter also
    // named "self".
    //
    // Fix round 1, Finding 1: pre_collect_class_body now placeholder-declares
    // (Unknown, at ITS OWN line) the first self.x = ... it finds scanning
    // EVERY method's body up front, so by the time this real, single-pass
    // walk reaches ANY self.x = ..., classes_.member_type already has_value()
    // for practically every attribute -- the OLD "does a member/method
    // already exist" test alone can no longer tell "brand new" apart from
    // "this IS that very placeholder, fill it in for real". The declared
    // LINE is the disambiguator, exactly like is_unfilled_placeholder's
    // ScopeStack analogue: a member whose declared_line equals THIS
    // statement's own line (and is not a method) is this statement's own
    // placeholder; anything else -- no member/method at all (an attribute
    // pre_collect_class_body's scan could not see, e.g. one first assigned
    // inside a nested def's own body) or a member at a DIFFERENT line (a
    // genuine earlier, real assignment) -- is handled below exactly as
    // before.
    if (const auto* receiver = dynamic_cast<const ast::Name*>(&target.value())) {
        if (receiver->identifier() == "self" && !current_class_qualified_name_.empty()) {
            const Resolution self_resolution = scopes_.resolve("self");
            if (self_resolution.binding != nullptr &&
                self_resolution.binding->type.kind == TypeKind::Class &&
                self_resolution.binding->type.name == current_class_qualified_name_) {
                const bool is_method_name =
                    classes_.method_type(current_class_qualified_name_, target.attribute()).has_value();
                const std::optional<int> existing_line =
                    classes_.member_declared_line(current_class_qualified_name_, target.attribute());
                const bool is_brand_new = !is_method_name && !existing_line.has_value();
                const bool is_own_placeholder = !is_method_name && existing_line.has_value() &&
                                                *existing_line == target.span().start_line;

                if (is_brand_new || is_own_placeholder) {
                    // Either the FIRST self.x = ... TypeChecker's own
                    // visitation has reached for this name (in THIS class; a
                    // base's member/method of the same name already fails
                    // is_brand_new and falls through to the ordinary path
                    // instead), or this exact statement's own placeholder
                    // from pre_collect_class_body -- infer the type from the
                    // value, exactly like an ordinary Name assignment, and
                    // declare it (overwriting the Unknown placeholder, in the
                    // latter case, with the real one). No comparison: there
                    // is nothing REAL yet to compare against either way.
                    const Type value_type = typer_.type_of(value, Type::unknown());
                    classes_.declare_member(current_class_qualified_name_, target.attribute(), value_type,
                                            target.span().start_line);
                    // type_of_attribute never ran for `target`, so its TypeMap
                    // entries would otherwise be missing -- recorded by hand,
                    // matching type_of_attribute's own class-object-receiver
                    // branch (expression_typer.cpp), which does the same for the
                    // same reason.
                    types_.insert(&target, value_type);
                    types_.insert(receiver, self_resolution.binding->type);
                    return;
                }
            }
        }
    }

    const Type value_type = typer_.type_of(value, Type::unknown());
    // Reuses type_of_attribute entirely, which already reports attr-defined
    // ("\"C\" has no attribute \"x\"") for a name the class never declares --
    // the attribute set is closed at the class definition, so assigning a
    // NEW attribute from outside the class is exactly that error. Also the
    // path a SECOND, conflicting self.x assignment falls through to (the
    // member now exists, from the first assignment above), matching
    // assign_name's own "first assignment's type is sticky" rule -- no join,
    // no union, just a compatibility check against the already-declared type.
    //
    // Fix round 2, Finding E: label corrected from "target" to "variable" --
    // verified against mypy 1.18.1 (both a same-class conflicting self.x
    // assignment and an outside-the-class `c.x = ...` reassignment) that an
    // attribute target's own incompatible-assignment message always reads
    // "variable has type ...", never "target has type ...", exactly like an
    // ordinary Name target's (assign_name already used "variable"; this was
    // the one call site left saying something else with no test pinning it).
    const Type member_type = typer_.type_of(target, Type::unknown());
    if (value_type.kind != TypeKind::Unknown && member_type.kind != TypeKind::Unknown &&
        !is_subtype(value_type, member_type, &classes_)) {
        report_incompatible_assignment(target, value_type, member_type, "variable");
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
        //
        // Fix round 1: a DIRECT class-body AnnAssign was already resolved
        // once by pre_collect_class_body's own eager pass -- reuse that
        // cached Type via bind_resolved_annotation (the scope-bind half of
        // bind_annotation) rather than invoking AnnotationResolver a second
        // time, which would double-report a bad annotation. One NESTED
        // inside an if/for within the class body (pre_collect_class_body
        // only scans the body directly, matching its own documented
        // simplification) is not in the cache and falls to the ordinary,
        // un-cached bind_annotation exactly as before.
        const auto cached_annotation = class_body_annotation_types_.find(&node);
        info = cached_annotation != class_body_annotation_types_.end()
                   ? bind_resolved_annotation(*target_name, cached_annotation->second,
                                              node.span().start_line)
                   : bind_annotation(*target_name, node.annotation(), node.span().start_line);
        if (!info.redefinition && scopes_.current_kind() == ScopeKind::Class) {
            // Task 19: a class-body AnnAssign ALSO declares an instance
            // attribute, in addition to the ordinary scope-bind above --
            // verified mypy accepts BOTH `C.x` and `c.x` for a bare `x: int`
            // class-body annotation with no value, so this runs regardless
            // of node.has_value() below. `current_kind() == Class` is true
            // for one directly in the body AND for one nested in an if/for
            // inside it (Python itself does not scope those), which is
            // exactly the set of positions mypy treats as class-body level.
            //
            // Fix round 1, Finding 5: guarded by has_value() exactly like
            // assign_attribute's own check -- without it, an EARLIER self.x
            // = ... assignment (in a method occurring ABOVE this annotation
            // in the class body) is silently re-typed with zero diagnostics,
            // since declare_member has no collision detection of its own.
            // (For a DIRECT class-body annotation -- the case reaching this
            // branch via the cache above -- pre_collect_class_body's own
            // identical guard already declared this exact member, so the
            // has_value() check below is true for THIS statement's own
            // declaration too; comparing info.type against itself is always
            // a clean no-op subtype check, so nothing is lost.)
            const bool already_member =
                classes_.member_type(current_class_qualified_name_, target_name->identifier())
                    .has_value();
            const bool already_method =
                classes_.method_type(current_class_qualified_name_, target_name->identifier())
                    .has_value();
            if (!already_member && !already_method) {
                classes_.declare_member(current_class_qualified_name_, target_name->identifier(),
                                        info.type, node.span().start_line);
            } else if (already_member) {
                const Type existing_type =
                    *classes_.member_type(current_class_qualified_name_, target_name->identifier());
                if (info.type.kind != TypeKind::Unknown && existing_type.kind != TypeKind::Unknown &&
                    !is_subtype(info.type, existing_type, &classes_)) {
                    report_incompatible_assignment(node, info.type, existing_type, "variable");
                }
            }
            // A same-name METHOD collision is left unreported here -- a
            // combined method+attribute namespace is a pre-existing gap
            // outside this fix round's scope.
        }
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
    // Task 19: "is this a method" collapsed into a single scope-kind check,
    // read BEFORE FunctionScopeGuard (below) pushes this def's OWN Function
    // scope -- so the CURRENT scope is still whatever this def is lexically
    // inside. True only when that is a Class scope: a method's own nested
    // def sees ScopeKind::Function instead (its enclosing method's
    // FunctionScopeGuard already pushed one), so it is correctly never a
    // method itself, with no separate reset needed (a prior, minimal
    // ClassDef override tracked a bool for exactly this and had to reset it
    // by hand for that same case; see type_checker.h's class-level comment).
    const bool is_method = scopes_.current_kind() == ScopeKind::Class;

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
        // No real return type was ever computed for this broken signature
        // (there is no self to build param_types from, so this whole
        // branch skips that machinery), so Return checks inside it get
        // Unknown -- absorbing, so a return statement here is never
        // (falsely) flagged on top of the signature error already reported
        // above. Task 20: the missing-return-statement check is also
        // skipped entirely for this branch, matching how it already skips
        // the "missing an annotation" completeness check just above.
        ReturnContextGuard return_guard(current_return_type_, Type::unknown());
        pre_bind_function_body(node.body());
        for (const ast::StmtPtr& statement : node.body()) {
            statement->accept(*this);
        }
        return;
    }

    // Every parameter's type, and whether it counts toward "missing an
    // annotation" -- self (a method's own first parameter) is exempt, per
    // mypy's disallow-untyped-defs. A top-level FunctionDef was already
    // resolved once by collect_signatures's Phase 2 (top_level_signatures_),
    // and a METHOD (fix round 1) by pre_collect_class_body's own pre-pass
    // (class_method_signatures_) -- reusing whichever cache has it is what
    // keeps AnnotationResolver from running -- and potentially
    // double-reporting a bad annotation -- a second time on the exact same
    // annotation expressions.
    std::vector<Type> param_types;
    Type return_type;
    bool any_param_missing = false;
    bool any_param_annotated = false;

    const Type* cached_signature = nullptr;
    const auto top_level_cached = top_level_signatures_.find(&node);
    if (top_level_cached != top_level_signatures_.end()) {
        cached_signature = &top_level_cached->second;
    } else {
        const auto method_cached = class_method_signatures_.find(&node);
        if (method_cached != class_method_signatures_.end()) {
            cached_signature = &method_cached->second;
        }
    }

    if (cached_signature != nullptr && !cached_signature->args.empty()) {
        // Task 18 fix round 1, Finding 6: `args.end() - 1`/`args.back()` are
        // safe TODAY -- Type::callable (both caching call sites' own caller)
        // always pushes the return, so a cached entry's args is never empty
        // -- but this cache is populated by a DIFFERENT function than the
        // one reading it, so nothing here proves that invariant holds by
        // construction the way type_of_positional_call's identical guard
        // (expression_typer_calls.cpp) does for a Callable built through the
        // exact same Type::callable call. Guarded rather than trusted, same
        // rationale as that guard's own comment.
        const Type& signature = *cached_signature;
        param_types.assign(signature.args.begin(), signature.args.end() - 1);
        return_type = signature.args.back();
        for (std::size_t i = 0; i < params.size(); ++i) {
            const ast::Parameter& parameter = params[i];
            if (parameter.annotation != nullptr) {
                any_param_annotated = true;
            } else if (is_method && i == 0) {
                // Fix round 1: a METHOD can now reach this cached branch too
                // (class_method_signatures_) -- self (index 0, unannotated)
                // is exempt from "missing an annotation" here exactly like
                // it already is in the uncached branch below. A top-level
                // def is never a method, so this exemption is simply unreached
                // for one, matching the ORIGINAL (now-corrected) comment's
                // intent.
            } else {
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
                // Task 19: self is bound to the ENCLOSING class, not Unknown
                // -- this is THE TRAP the brief warns about. Unknown is
                // absorbing, so before this change `self.a`/`self.m()` were
                // silently accepted no matter what; landing this alone (with
                // no attribute collection alongside it) would flip
                // InitNeedsNoReturnAnnotationWhenAParameterIsAnnotated's
                // `self.a = a` into a false attr-defined TypeError, which is
                // exactly why assign_attribute's new declare-on-first-
                // assignment path had to land in this SAME change.
                param_types.push_back(Type::class_of(current_class_qualified_name_));
            } else {
                param_types.push_back(Type::unknown());
                any_param_missing = true;
            }
        }
        return_type = node.has_return_annotation() ? resolver.resolve(node.return_annotation())
                                                    : Type::unknown();
    }

    if (is_method) {
        // Task 19: a method's signature -- self INCLUDED, per
        // ClassTable::method_type's own contract -- is declared into
        // ClassTable as soon as it is known, which is also what makes
        // __init__ discoverable as a constructor (ClassTable::constructor_
        // type looks for a method literally named "__init__"). Never bound
        // into ScopeStack: see the class-level comment on why a class's (and
        // now a method's) own name must not be.
        classes_.declare_method(current_class_qualified_name_, node.name(),
                                Type::callable(param_types, return_type));
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
            // Fix round 2: routed through is_unfilled_placeholder for
            // consistency with every other same-line-rebind site, though the
            // order_exempt guard is unreachable here in practice -- an
            // order_exempt binding is only ever a PARAMETER, whose
            // declared_line is the ENCLOSING def's own header line, and a
            // nested `def` (a compound statement) can never share that exact
            // line: Python's grammar requires it to start its own indented
            // statement line, never trail a `:` inline. Kept as the shared
            // helper anyway rather than the raw comparison, so a future
            // change to either rule only has one place to update.
            if (is_unfilled_placeholder(*existing.binding, def_line)) {
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
    // Task 20: `return_type` (resolved above, either freshly or from
    // top_level_signatures_'s cache) becomes the declared type Return's own
    // checks read for the DURATION of this body walk -- restored by
    // ReturnContextGuard's destructor to whatever the ENCLOSING function's
    // (if any) was, so a nested def checks its own returns against its own
    // signature.
    ReturnContextGuard return_guard(current_return_type_, return_type);
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

    // Task 20's return-path check, run once per FunctionDef at the very end
    // of its own body walk -- ONLY when the declared return type is neither
    // None (nothing to return, so fall-through is fine) nor Unknown (no
    // reliable annotation to enforce; the earlier any_param_missing/
    // return_missing check already flags a genuinely missing one). Reported
    // at the `def` line, matching mypy, which reports this at the function's
    // own definition rather than at the fall-through point.
    if (node.has_return_annotation() && return_type.kind != TypeKind::NoneType &&
        return_type.kind != TypeKind::Unknown && !always_returns(node.body())) {
        report(node, "TypeError", "missing return statement");
    }
}

void TypeChecker::visit(const ast::ClassDef& node) {
    // Task 19: a class body is a REAL, order-sensitive ScopeKind::Class push
    // -- a forward reference within it (`a: int = b` before `b` is declared)
    // resolves to NOTHING (no placeholder is ever pre-bound for a class-body
    // name, unlike module/function scope's own pre-bind passes), so it falls
    // straight through to type_of_name's ordinary not-found path and reports
    // plain NameError, matching mypy's own name-defined wording for this
    // exact mistake rather than the used-before-def wording module scope
    // gets for the structurally identical case.
    //
    // Bases are still walked first (matching RecursiveVisitor::visit's own
    // bases-then-body order, no longer delegated to since only the body half
    // needs the new scope): TypeChecker overrides no Name/Attribute visit,
    // so this remains inert today, exactly as before this task.
    for (const ast::ExprPtr& base : node.bases()) {
        base->accept(*this);
    }

    std::string qualified_name;
    if (collided_top_level_.count(&node) != 0) {
        // Fix round 1, Finding 7: this top-level ClassDef is a LOSING
        // same-name collision scan_top_level_names already reported --
        // collect_classes' Phase 1 skipped its declare() call, so it has no
        // entry under its own bare name at all. Isolate it under a name
        // nothing else can ever resolve to, rather than falling through to
        // the bare-name branch below (which would silently merge its
        // members/methods, and possibly its __init__, onto the WINNING
        // same-named class's entry). See declare_isolated_class's own
        // comment.
        qualified_name = declare_isolated_class(
            node, "<shadowed-class>#" + std::to_string(node.span().start_line) + "#" + node.name());
    } else if (scopes_.current_kind() != ScopeKind::Module && scopes_.current_kind() != ScopeKind::Class) {
        // Fix round 1, Finding 4: a ClassDef lexically inside a `def` (or any
        // other non-module, non-class scope) was never seen by
        // collect_classes' Phase-1 walk either, for the identical reason.
        //
        // Fix round 2, Finding B: round 1's own fix declared a NON-colliding
        // local class under its own BARE name -- but `declare()` has no
        // collision detection of its own, so a SECOND, later-declared local
        // class of the identical name (in a DIFFERENT function, or a second
        // call to the SAME function-shaped class-factory pattern) silently
        // OVERWROTE the first one's ClassTable entry. `L()` from inside the
        // second function's own body then resolved to the FIRST function's
        // class -- not "no longer a constructor call", but the WRONG class
        // -- and a member access on the result was a FALSE attr-defined
        // TypeError, exactly the hard invariant this project exists to
        // protect. Worse, a local class's bare name is then a LIVE
        // ClassTable entry for the REST of the module's Phase-3 walk (Phase 1
        // never runs for one of these, so nothing ever removes the entry),
        // so a module-level `L()` occurring TEXTUALLY AFTER the function
        // that declares `class L` went from a correct NameError to a silent
        // false acceptance.
        //
        // Fixed by ALWAYS isolating a local ClassDef under a synthetic,
        // per-declaration-site qualified name embedding '#' (a character no
        // Python identifier can ever contain) -- never the bare name, even
        // when nothing else currently uses it. The accepted cost: `Local()`
        // (a bare-Name call) can no longer be resolved as a constructor
        // call, even from inside its OWN defining function --
        // classes_.is_class(node.name()) is now permanently false for one of
        // these, since type_of_name_call's constructor dispatch
        // (expression_typer_calls.cpp) has no scope awareness at all and
        // looks up the literal source-level identifier -- so this now
        // reports a plain NameError instead of constructing. A MISSED error
        // beats a FALSE one: the class's own attributes are still correctly
        // collected (self.x = ... inside its methods still declares members
        // exactly as before), so a local class used only for its side
        // effects, or never referenced by a bare call at all, is entirely
        // unaffected. Scope-aware constructor dispatch would avoid even this
        // cost, but that dispatch lives in ExpressionTyper
        // (expression_typer_calls.cpp), a different file outside this fix
        // round's assigned scope, and is judged out of proportion for a
        // construct this rare.
        qualified_name = declare_isolated_class(
            node, "<local-class>#" + std::to_string(node.span().start_line) + "#" + node.name());
    } else {
        qualified_name = current_class_qualified_name_.empty()
                             ? node.name()
                             : current_class_qualified_name_ + "." + node.name();
        // Bases and the class itself were already declared, under this SAME
        // qualified name, by collect_classes's declare_class_recursive (Phase
        // 1) -- so member/method declaration below has an Entry to write
        // into, and a forward reference to a class declared later in the
        // same module (or a differently-nested one) already resolves.
    }

    ClassContextGuard guard(scopes_, current_class_qualified_name_, qualified_name);
    // Fix round 1, Finding 1: declares every method signature and
    // placeholder-declares every attribute BEFORE any of this class's own
    // body statements are walked for real -- see pre_collect_class_body's
    // own comment for the full mechanism.
    pre_collect_class_body(node, qualified_name);
    for (const ast::StmtPtr& statement : node.body()) {
        statement->accept(*this);
    }
}

void TypeChecker::pre_collect_class_body(const ast::ClassDef& node,
                                         const std::string& qualified_name) {
    // Fix round 2, Finding E: every direct class-body AnnAssign is resolved
    // and declared in its OWN sub-pass, over the WHOLE body, strictly BEFORE
    // any method's self.x scan runs below -- verified against mypy 1.18.1:
    // a class-body annotation is the DECLARED type of that attribute for the
    // WHOLE class body regardless of where it appears textually (`x: str`
    // BELOW an earlier `self.x = 5` still makes "x" a str variable, and the
    // conflict is reported at the ASSIGNMENT's own line, never the
    // annotation's -- see AClassBodyAnnotationConflictingWithAnEarlierSelf
    // AssignmentIsReported), matching how mypy treats every other flat
    // (Python has no block scoping) variable declaration. This is safe to
    // resolve eagerly and order-independently -- unlike a plain Assign's
    // inferred type below, which stays deferred to Phase 3's own
    // order-sensitive walk -- because an annotation's type comes from the
    // ANNOTATION EXPRESSION alone (AnnotationResolver, which depends only on
    // ClassTable, already fully populated by Phase 1), never from a VALUE
    // expression that could itself forward-reference another not-yet-bound
    // class-body name.
    for (const ast::StmtPtr& statement : node.body()) {
        if (const auto* ann_assign = dynamic_cast<const ast::AnnAssign*>(statement.get())) {
            if (const auto* target_name = dynamic_cast<const ast::Name*>(&ann_assign->target())) {
                AnnotationResolver resolver(classes_, sink_);
                const Type type = resolver.resolve(ann_assign->annotation());
                class_body_annotation_types_.emplace(ann_assign, type);
                // Fix round 1, Finding 5: guarded exactly like assign_attribute's
                // own has_value() check -- a SECOND class-body AnnAssign
                // under the same name (the only remaining way `already_member`
                // can be true here, now that this sub-pass runs before any
                // self.x placeholder ever could) is left as the first
                // occurrence's declaration; declare_member has no collision
                // detection of its own, so declaring over it unconditionally
                // would silently re-type the attribute with zero diagnostics.
                if (!classes_.member_type(qualified_name, target_name->identifier()).has_value() &&
                    !classes_.method_type(qualified_name, target_name->identifier()).has_value()) {
                    classes_.declare_member(qualified_name, target_name->identifier(), type,
                                            ann_assign->span().start_line);
                }
            }
        }
    }

    for (const ast::StmtPtr& statement : node.body()) {
        if (const auto* function_def = dynamic_cast<const ast::FunctionDef*>(statement.get())) {
            if (function_def->params().empty()) {
                // A method with no parameters at all (missing self) is
                // reported by visit(FunctionDef) itself, which never
                // registers it in ClassTable either -- this pre-pass must
                // not add signature information for a method that will
                // never really have one.
                continue;
            }
            const Type signature = resolve_method_signature(*function_def, qualified_name);
            class_method_signatures_.emplace(function_def, signature);
            classes_.declare_method(qualified_name, function_def->name(), signature);
            collect_self_attribute_placeholders(qualified_name, function_def->body());
        } else if (const auto* assign = dynamic_cast<const ast::Assign*>(statement.get())) {
            if (const auto* target_name = dynamic_cast<const ast::Name*>(&assign->target())) {
                // Fix round 1, Finding 3: a plain class-body Assign
                // placeholder-declares the SAME way self.x = ... does (real
                // type filled in later, by assign_to's own is_new_definition
                // check, when Phase 3 actually reaches this statement) --
                // NOT eagerly typed here, since its value expression may
                // itself reference another class-body name whose real,
                // order-sensitive resolution must stay entirely within
                // Phase 3's single pass.
                if (!classes_.member_type(qualified_name, target_name->identifier()).has_value() &&
                    !classes_.method_type(qualified_name, target_name->identifier()).has_value()) {
                    classes_.declare_member(qualified_name, target_name->identifier(), Type::unknown(),
                                            assign->span().start_line);
                }
            }
        }
        // A nested ClassDef is handled entirely by its OWN visit(ClassDef)
        // call, when Phase 3's real walk reaches it -- nothing to pre-collect
        // for one here.
    }
}

Type TypeChecker::resolve_method_signature(const ast::FunctionDef& method,
                                           const std::string& qualified_name) {
    AnnotationResolver resolver(classes_, sink_);
    const std::vector<ast::Parameter>& params = method.params();
    std::vector<Type> param_types;
    param_types.reserve(params.size());
    for (std::size_t i = 0; i < params.size(); ++i) {
        const ast::Parameter& parameter = params[i];
        if (parameter.annotation != nullptr) {
            param_types.push_back(resolver.resolve(*parameter.annotation));
        } else if (i == 0) {
            // The ORIGINAL Task 19 "self upgrade": bound to the enclosing
            // class, not Unknown.
            param_types.push_back(Type::class_of(qualified_name));
        } else {
            param_types.push_back(Type::unknown());
        }
    }
    const Type return_type = method.has_return_annotation() ? resolver.resolve(method.return_annotation())
                                                             : Type::unknown();
    return Type::callable(param_types, return_type);
}

void TypeChecker::collect_self_attribute_placeholders(const std::string& qualified_name,
                                                       const std::vector<ast::StmtPtr>& body) {
    for (const ast::StmtPtr& statement : body) {
        if (const auto* assign = dynamic_cast<const ast::Assign*>(statement.get())) {
            if (const auto* attribute = dynamic_cast<const ast::Attribute*>(&assign->target())) {
                if (const auto* receiver = dynamic_cast<const ast::Name*>(&attribute->value())) {
                    if (receiver->identifier() == "self" &&
                        !classes_.member_type(qualified_name, attribute->attribute()).has_value() &&
                        !classes_.method_type(qualified_name, attribute->attribute()).has_value()) {
                        classes_.declare_member(qualified_name, attribute->attribute(), Type::unknown(),
                                                assign->span().start_line);
                    }
                }
            }
        } else if (const auto* if_stmt = dynamic_cast<const ast::If*>(statement.get())) {
            collect_self_attribute_placeholders(qualified_name, if_stmt->body());
            collect_self_attribute_placeholders(qualified_name, if_stmt->orelse());
        } else if (const auto* while_stmt = dynamic_cast<const ast::While*>(statement.get())) {
            collect_self_attribute_placeholders(qualified_name, while_stmt->body());
            collect_self_attribute_placeholders(qualified_name, while_stmt->orelse());
        } else if (const auto* for_stmt = dynamic_cast<const ast::For*>(statement.get())) {
            collect_self_attribute_placeholders(qualified_name, for_stmt->body());
            collect_self_attribute_placeholders(qualified_name, for_stmt->orelse());
        }
        // A nested FunctionDef/ClassDef is a new scope -- 'self' there may be
        // shadowed or simply absent -- so it is out of this scan's reach,
        // matching pre_bind_function_body's own scope boundary.
    }
}

void TypeChecker::visit(const ast::If& node) {
    // Truthiness is universal -- ANY condition type is acceptable, so unlike
    // every other typed subexpression in this checker there is no
    // compatibility check to run against the result at all; it is typed
    // purely so the TypeMap stays complete and any error INSIDE the
    // condition (an unbound name, say) still reports.
    typer_.set_statement_line(node.span().start_line);
    typer_.type_of(node.condition(), Type::unknown());
    for (const ast::StmtPtr& statement : node.body()) {
        statement->accept(*this);
    }
    for (const ast::StmtPtr& statement : node.orelse()) {
        statement->accept(*this);
    }
}

void TypeChecker::visit(const ast::While& node) {
    // Same reasoning as If: truthiness is universal, so the condition is
    // typed and never checked against anything.
    typer_.set_statement_line(node.span().start_line);
    typer_.type_of(node.condition(), Type::unknown());
    for (const ast::StmtPtr& statement : node.body()) {
        statement->accept(*this);
    }
    for (const ast::StmtPtr& statement : node.orelse()) {
        statement->accept(*this);
    }
}

void TypeChecker::visit(const ast::For& node) {
    const int line = node.span().start_line;
    typer_.set_statement_line(line);
    const Type iterable_type = typer_.type_of(node.iterable(), Type::unknown());
    // Routed through ExpressionTyper::element_type_of -- the SAME apply()
    // switch type_of_list_comp uses for its own, identical need -- rather
    // than a second copy of the three-way RuleResult handling here.
    const Type element = typer_.element_type_of(node.iterable(), iterable_type);

    if (const auto* tuple_target = dynamic_cast<const ast::TupleExpr*>(&node.target())) {
        // mypy ACCEPTS a tuple target (`for a, b in pairs:` is mypy-clean),
        // so this must be NotImplementedError, not TypeError -- element_type
        // of a tuple[K, V] is the UNION K | V, not a positional pair, so
        // there is nothing correct to bind a/b to element-wise. Mirrors
        // type_of_list_comp's identical tuple-target arm.
        report(*tuple_target, "NotImplementedError",
              "tuple targets in for loops are not supported");
        // Fix round 1, Finding 8: bind each element to Unknown anyway, the
        // same precedent assign_tuple's own arity-mismatch fallback sets --
        // otherwise the body is still walked (unlike a `pass`-only fixture, a
        // real body reading `a`/`b` here would see them wholly UNBOUND) and a
        // real use cascades its own NameError on top of this
        // NotImplementedError instead of being silently absorbed like every
        // other "unsupported shape" case in this checker. order_exempt=true
        // for the same reason the Name-target arm below needs it: bound
        // before the body runs, so a one-line suite reading one right back
        // can never be a genuine use-before-definition.
        for (const ast::ExprPtr& element : tuple_target->elements()) {
            if (const auto* name = dynamic_cast<const ast::Name*>(element.get())) {
                if (!scopes_.bound_in_current_scope(name->identifier())) {
                    scopes_.bind(name->identifier(),
                                 Binding{Type::unknown(), line, /*annotated=*/false,
                                        /*order_exempt=*/true});
                }
            }
        }
    } else if (const auto* name_target = dynamic_cast<const ast::Name*>(&node.target())) {
        // A for target does NOT get its own scope (unlike a comprehension's
        // Comprehension scope) -- bound via assign_name, exactly like an
        // ordinary Name assignment, into the CURRENT scope, so it survives
        // the loop and a reassignment through a second loop is checked for
        // compatibility rather than silently rebound.
        //
        // order_exempt=true (fix round 1, Finding 1, CRITICAL): a for target
        // is bound before its OWN body ever runs, exactly like a parameter --
        // so a one-line suite (`for i in range(3): print(i)`) reading it
        // within that same body is never a genuine use-before-definition.
        // Without this, the body's ExprStmt sets statement_line_ to this same
        // line, and the ordinary `declared_line >= statement_line_` ordering
        // check (declared_line == line here too) misfires exactly like it did
        // for a one-line def's own parameter before Task 18 fixed that case.
        // A read BEFORE the loop is untouched by this: the name is not bound
        // at all yet, so it still correctly reports "is not defined".
        assign_name(*name_target, element, line, /*order_exempt=*/true);
    }
    // Any other target shape (Attribute, Subscript) is outside this task's
    // tested scope; nothing to bind.

    for (const ast::StmtPtr& statement : node.body()) {
        statement->accept(*this);
    }
    for (const ast::StmtPtr& statement : node.orelse()) {
        statement->accept(*this);
    }
}

void TypeChecker::visit(const ast::Return& node) {
    const int line = node.span().start_line;
    typer_.set_statement_line(line);

    if (!node.has_value()) {
        // A bare `return` is only wrong when the function's declared return
        // type demands a value -- i.e. it is neither None (nothing expected)
        // nor Unknown (no reliable declared type to enforce at all).
        if (current_return_type_.kind != TypeKind::Unknown &&
            current_return_type_.kind != TypeKind::NoneType) {
            report(node, "TypeError", "return value expected");
        }
        return;
    }

    // The declared return type is the value's EXPECTED CONTEXT, exactly like
    // an AnnAssign's declared type -- `def f() -> list[int]: return []`
    // types the bare `[]` against list[int] rather than Unknown.
    const Type value_type = typer_.type_of(node.value(), current_return_type_);
    if (current_return_type_.kind == TypeKind::Unknown) {
        // No reliable declared type to check the value against.
        return;
    }
    if (current_return_type_.kind == TypeKind::NoneType) {
        report(node, "TypeError", "no return value expected");
        return;
    }
    if (value_type.kind != TypeKind::Unknown &&
        !is_subtype(value_type, current_return_type_, &classes_)) {
        // New wording, not in the corpus's pre-existing settled set --
        // verified against mypy 1.18.1's own "Incompatible return value type
        // (got \"str\", expected \"int\")", lower-cased to match this
        // codebase's existing message-casing convention (every other message
        // here starts lower-case despite mypy's own title case).
        report(node, "TypeError",
              "incompatible return value type (got \"" + type_name(value_type) +
                  "\", expected \"" + type_name(current_return_type_) + "\")");
    }
}

bool TypeChecker::contains_reachable_break(const std::vector<ast::StmtPtr>& body) {
    for (const ast::StmtPtr& statement : body) {
        if (dynamic_cast<const ast::Break*>(statement.get()) != nullptr) {
            return true;
        }
        if (const auto* if_stmt = dynamic_cast<const ast::If*>(statement.get())) {
            // An `if` is not a loop, so a break inside one still belongs to
            // THIS enclosing loop -- look through it.
            if (contains_reachable_break(if_stmt->body()) ||
                contains_reachable_break(if_stmt->orelse())) {
                return true;
            }
            continue;
        }
        // Fix round 1, Finding 3: a nested For/While's own BODY is
        // deliberately not recursed into -- a break there can only ever
        // escape THAT loop, never this one. But its ORELSE is the opposite
        // case: a loop's `else` clause runs OUTSIDE the loop's own break
        // scope (that is precisely why `for x in []: pass` / `else: break`
        // is a top-level `SyntaxError: 'break' outside loop` -- the `else`
        // is not inside the loop it is attached to), so a `break` written
        // there targets the ENCLOSING loop and must count here. The previous
        // version of this comment claimed a break in ANY nested loop
        // construct "can only ever escape that inner loop" -- true of the
        // body, false of the orelse, and this is the fix for that false
        // claim.
        if (const auto* for_stmt = dynamic_cast<const ast::For*>(statement.get())) {
            if (contains_reachable_break(for_stmt->orelse())) {
                return true;
            }
            continue;
        }
        if (const auto* while_stmt = dynamic_cast<const ast::While*>(statement.get())) {
            if (contains_reachable_break(while_stmt->orelse())) {
                return true;
            }
            continue;
        }
        // FunctionDef/ClassDef bodies are new scopes a `break` cannot reach
        // out of at all (and could not legally appear there either), so they
        // are skipped.
    }
    return false;
}

bool TypeChecker::always_returns(const std::vector<ast::StmtPtr>& body) {
    for (const ast::StmtPtr& statement : body) {
        if (dynamic_cast<const ast::Return*>(statement.get()) != nullptr) {
            return true;
        }
        if (const auto* if_stmt = dynamic_cast<const ast::If*>(statement.get())) {
            if (!if_stmt->orelse().empty() && always_returns(if_stmt->body()) &&
                always_returns(if_stmt->orelse())) {
                return true;
            }
            continue;
        }
        if (const auto* while_stmt = dynamic_cast<const ast::While*>(statement.get())) {
            if (is_literal_true(while_stmt->condition()) &&
                !contains_reachable_break(while_stmt->body())) {
                return true;
            }
            continue;
        }
        // A For, or a While with any other condition, is assumed skippable
        // (false) -- the syntactic approximation the brief settles on. This
        // can only ever answer false where mypy answers true (a missed
        // error), never the reverse.
    }
    return false;
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
