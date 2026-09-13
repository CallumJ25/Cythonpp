#include "type_checker.h"

#include <cassert>
#include <cstddef>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "annotation_resolver.h"
#include "builtin_call_table.h"
#include "domain/ast/attribute.h"
#include "domain/ast/break.h"
#include "domain/ast/call.h"
#include "domain/ast/constant.h"
#include "domain/ast/continue.h"
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
// -- mirrors ExpressionTyper's ComprehensionScopeGuard
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

// RAII guard for one function body's worth of scope-limited class aliases:
// pushes an empty frame onto
// local_class_alias_frames_ alongside the FunctionScopeGuard that pushes that
// body's own ScopeKind::Function, and on destruction undoes every alias
// visit(ClassDef) registered in it -- in REVERSE order, RESTORING each bare
// name's previous scoped target rather than deleting it outright, so an inner
// function's own same-named local class shadows an outer one's for exactly
// its own body: `def outer: class L; def inner: class L; ...; L()` must still
// see OUTER's L at that trailing call, not nothing at all.
class LocalClassAliasGuard {
public:
    LocalClassAliasGuard(ClassTable& classes, std::vector<LocalClassAliasFrame>& frames)
        : classes_(classes), frames_(frames) {
        frames_.emplace_back();
    }
    ~LocalClassAliasGuard() {
        const LocalClassAliasFrame& frame = frames_.back();
        for (auto it = frame.rbegin(); it != frame.rend(); ++it) {
            if (it->second.has_value()) {
                classes_.declare_scoped_alias(it->first, *it->second);
            } else {
                classes_.remove_scoped_alias(it->first);
            }
        }
        frames_.pop_back();
    }

    LocalClassAliasGuard(const LocalClassAliasGuard&) = delete;
    LocalClassAliasGuard& operator=(const LocalClassAliasGuard&) = delete;

private:
    ClassTable& classes_;
    std::vector<LocalClassAliasFrame>& frames_;
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

// Sets a bool for the duration of a body walk and puts the previous value
// back, the same shape as ReturnContextGuard above and for the same reason:
// visit(FunctionDef) has a report-and-return path between the set and the
// restore, so RAII is the only safe pairing.
class FlagGuard {
public:
    FlagGuard(bool& current, bool new_value) : current_(current), previous_(current) {
        current_ = new_value;
    }
    ~FlagGuard() { current_ = previous_; }

    FlagGuard(const FlagGuard&) = delete;
    FlagGuard& operator=(const FlagGuard&) = delete;

private:
    bool& current_;
    bool previous_;
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

// Structural type identity with Union members matched as an unordered SET at
// every depth -- i.e. exactly what `Type::operator==` computes, minus its
// union-order sensitivity, and nothing else relaxed. The sole caller is
// TypeChecker::has_identical_signature, whose header comment records why
// neither `operator==` nor `is_equivalent` answers this question.
//
// Recursion into `args` is load-bearing, not defensive: measured 2026-09-11
// with mypy 1.18.1, conditional variants `def g(a: list[int | str])` and
// `def g(a: list[str | int])` are accepted, and so are the same pair inside
// `dict`'s key position, inside a `tuple` element, and two levels deep
// (`list[dict[int | str, list[bool | float]]]`). A top-level-only check would
// have left every one of those a false positive.
//
// The unordered match consumes each right-hand member at most once, rather
// than just testing one-way containment at equal sizes. Type::union_of
// de-duplicates with `operator==`, which is order-SENSITIVE, so a union can
// legitimately hold two members that are distinct under == yet identical
// here (`list[int | str] | list[str | int]`) -- and with those present,
// containment-plus-equal-size would call {A, A'} and {A, B} identical.
bool same_type_up_to_union_order(const Type& left, const Type& right) {
    if (left.kind != right.kind || left.name != right.name ||
        left.defaulted_params != right.defaulted_params ||
        left.args.size() != right.args.size()) {
        return false;
    }
    if (left.kind == TypeKind::Union) {
        std::vector<bool> matched(right.args.size(), false);
        for (const Type& member : left.args) {
            bool found = false;
            for (std::size_t index = 0; index < right.args.size(); ++index) {
                if (!matched[index] && same_type_up_to_union_order(member, right.args[index])) {
                    matched[index] = true;
                    found = true;
                    break;
                }
            }
            if (!found) {
                return false;
            }
        }
        return true;
    }
    for (std::size_t index = 0; index < left.args.size(); ++index) {
        if (!same_type_up_to_union_order(left.args[index], right.args[index])) {
            return false;
        }
    }
    return true;
}

// Invokes `visit(name)` for every bare Name an assignment target binds --
// itself a Name, or a TupleExpr of them (`a, b = ...`, including a nested
// TupleExpr, `(self, x), y = ...`) -- because unpacking assignment can rebind
// a name just as plainly as a direct one (`self, x = Bag(), 1` rebinds `self`
// exactly like `self = Bag()` does).
void for_each_bound_name(const ast::Expr& target,
                        const std::function<void(const std::string&)>& visit_name) {
    if (const auto* bare_name = dynamic_cast<const ast::Name*>(&target)) {
        visit_name(bare_name->identifier());
        return;
    }
    if (const auto* tuple = dynamic_cast<const ast::TupleExpr*>(&target)) {
        for (const ast::ExprPtr& element : tuple->elements()) {
            for_each_bound_name(*element, visit_name);
        }
    }
}

// What kind of statement bound a name, for for_each_own_scope_binding's
// callers to filter by: receiver_rebound_in_own_scope treats every kind as a
// shadow (a class's own name rebinds exactly as an assignment does -- see its
// own comment), while pre_bind_function_body deliberately excludes
// NestedClass -- see pre_bind_function_body's own comment for why.
enum class OwnScopeBindingKind { Assign, AnnAssign, ForTarget, NestedDef, NestedClass };

// Invokes `visit(name, line, kind)` for every name bound directly in `body`'s
// own scope: an ordinary assignment, an annotated assignment, a `for` target
// (Name or TupleExpr), or a nested def/class's own name. Python rebinds a
// name in a function's scope by ANY of these, not just by appearing as a
// parameter: `def inner(): self = Bag(); self.q = 1` makes `self` a local of
// `inner` exactly as a parameter named `self` would, and mypy correctly
// refuses to attribute that store to the enclosing method's own receiver (or,
// for pre_bind_function_body's own use, correctly reports a same-scope read
// above the rebind as "used before definition" rather than resolving it
// outward).
//
// Recurses into If/While/For bodies and their `else` clauses (not new
// scopes). A nested FunctionDef or ClassDef is two DIFFERENT things, not one:
// its BODY is a different scope and is never descended into (a name bound
// inside THAT body is a different scope's own local and does not rebind this
// one), but `def self(): ...` / `class self: ...` ITSELF binds "self" in the
// ENCLOSING scope -- the same scope this function is scanning -- exactly as
// `self = Bag()` does. Conflating those two questions -- "does the body
// rebind the name" versus "does the definition's own name rebind it" -- is
// exactly what made `class self: pass` followed by `self.q = 1` (or the
// `def self(): pass` sibling) completely silent for receiver_rebound_in_own_
// scope's original purpose (2026-09-12): mypy reports attr-defined at both
// the read above and the store itself (CPython raises AttributeError), and
// that scan, checking only Assign/AnnAssign/For at the time, reported
// nothing at all, because the pre-pass still thought "self" was unshadowed
// and placeholder-declared "q" on Bag.
//
// Augmented assignment to a target (`self += 1`) needs no arm here: the
// parser rejects it outright with its own SyntaxError before this scan ever
// runs, so there is no silent case to cover.
void for_each_own_scope_binding(
    const std::vector<ast::StmtPtr>& body,
    const std::function<void(const std::string&, int, OwnScopeBindingKind)>& visit) {
    for (const ast::StmtPtr& statement : body) {
        if (const auto* assign = dynamic_cast<const ast::Assign*>(statement.get())) {
            const int line = assign->span().start_line;
            for_each_bound_name(assign->target(), [&](const std::string& name) {
                visit(name, line, OwnScopeBindingKind::Assign);
            });
        } else if (const auto* ann_assign = dynamic_cast<const ast::AnnAssign*>(statement.get())) {
            if (const auto* target_name = dynamic_cast<const ast::Name*>(&ann_assign->target())) {
                visit(target_name->identifier(), ann_assign->span().start_line,
                      OwnScopeBindingKind::AnnAssign);
            }
        } else if (const auto* for_stmt = dynamic_cast<const ast::For*>(statement.get())) {
            const int line = for_stmt->span().start_line;
            for_each_bound_name(for_stmt->target(), [&](const std::string& name) {
                visit(name, line, OwnScopeBindingKind::ForTarget);
            });
            for_each_own_scope_binding(for_stmt->body(), visit);
            for_each_own_scope_binding(for_stmt->orelse(), visit);
        } else if (const auto* if_stmt = dynamic_cast<const ast::If*>(statement.get())) {
            for_each_own_scope_binding(if_stmt->body(), visit);
            for_each_own_scope_binding(if_stmt->orelse(), visit);
        } else if (const auto* while_stmt = dynamic_cast<const ast::While*>(statement.get())) {
            for_each_own_scope_binding(while_stmt->body(), visit);
            for_each_own_scope_binding(while_stmt->orelse(), visit);
        } else if (const auto* nested_def = dynamic_cast<const ast::FunctionDef*>(statement.get())) {
            visit(nested_def->name(), nested_def->span().start_line, OwnScopeBindingKind::NestedDef);
            // Its BODY is a different scope -- deliberately not descended
            // into, matching every other reason this file stops at that
            // boundary.
        } else if (const auto* nested_class = dynamic_cast<const ast::ClassDef*>(statement.get())) {
            visit(nested_class->name(), nested_class->span().start_line,
                  OwnScopeBindingKind::NestedClass);
            // Its BODY is a different scope, for the same reason.
        }
    }
}

// True if `name` is bound anywhere in `body`'s own scope -- see
// for_each_own_scope_binding for exactly which statement forms count and
// where the recursion stops. NOTE this closes the placeholder half only:
// ScopeStack deliberately never binds a class's own name (see "Class names
// are deliberately NOT bound into ScopeStack" elsewhere in this file), so
// self_attribute_receiver_type's own real-walk resolution still cannot see a
// `class self: ...` shadow either -- the STORE itself may still go
// unreported for that separate, structural reason, but the pre-pass no
// longer manufactures a placeholder that hides the READ above it.
bool receiver_rebound_in_own_scope(const std::string& name, const std::vector<ast::StmtPtr>& body) {
    bool shadowed = false;
    for_each_own_scope_binding(body,
                              [&](const std::string& bound_name, int, OwnScopeBindingKind) {
        if (bound_name == name) {
            shadowed = true;
        }
    });
    return shadowed;
}

} // namespace

TypeChecker::TypeChecker(diagnostics::DiagnosticSink& sink)
    : sink_(sink), scopes_(), classes_(), types_(), narrowings_(),
      typer_(scopes_, classes_, types_, narrowings_, sink_) {}

TypeMap TypeChecker::check(const ast::Module& module) {
    module.accept(*this);
    return std::move(types_);
}

void TypeChecker::visit(const ast::Module& node) {
    scan_top_level_names(node);
    collect_classes(node);
    // MEMBER COLLECTION, for EVERY class at every nesting depth, BEFORE any
    // annotation is resolved and before any statement is checked. Previously
    // a class's members were collected when the ordinary walk reached that
    // class's own ClassDef, so any reference from ABOVE it saw a class with
    // no members at all -- `class Cache: def use(self): return Item().n`
    // above `class Item` was a false attr-defined TypeError on code mypy
    // accepts and CPython runs (a method body does not execute at
    // class-definition time). Running here rather than inside
    // declare_class_recursive keeps the ordering that matters: every class in
    // the module is DECLARED first, so an annotation inside any class body
    // can name any other class regardless of textual order.
    for (const ClassDeclaration& declaration : declared_classes_) {
        pre_collect_class_body(*declaration.node, declaration.qualified_name);
    }
    collect_signatures(node);
    pre_bind_assignment_targets(node);
    check_suite(node.body());
}

void TypeChecker::for_each_flat_statement(
    const std::vector<ast::StmtPtr>& body, bool directly_in_body,
    const std::function<void(const ast::Stmt&, bool)>& visitor) {
    for (const ast::StmtPtr& statement : body) {
        visitor(*statement, directly_in_body);
        if (const auto* if_stmt = dynamic_cast<const ast::If*>(statement.get())) {
            for_each_flat_statement(if_stmt->body(), false, visitor);
            for_each_flat_statement(if_stmt->orelse(), false, visitor);
        } else if (const auto* while_stmt = dynamic_cast<const ast::While*>(statement.get())) {
            for_each_flat_statement(while_stmt->body(), false, visitor);
            for_each_flat_statement(while_stmt->orelse(), false, visitor);
        } else if (const auto* for_stmt = dynamic_cast<const ast::For*>(statement.get())) {
            for_each_flat_statement(for_stmt->body(), false, visitor);
            for_each_flat_statement(for_stmt->orelse(), false, visitor);
        }
        // A FunctionDef's or ClassDef's own body is a DIFFERENT scope and is
        // deliberately not descended into -- see the header.
    }
}

void TypeChecker::scan_top_level_names(const ast::Module& module) {
    // Recursed through control flow (see for_each_flat_statement): a class or
    // def inside an `if` binds its name in module scope, so a same-named
    // definition there collides with a flat one exactly as two flat ones do.
    //
    // BUT ONLY WHEN A CLASS IS INVOLVED. Measured against mypy 1.18.1, all
    // seven arrangements:
    //   flat def    + flat def           -> `Name "f" already defined` [no-redef]
    //   flat def    + def inside an if   -> Success
    //   def in if   + def in same if     -> Success
    //   def in if   + flat def           -> `Name "f" already defined` [no-redef]
    //   flat class  + class inside an if -> `Name "Bag" already defined`
    //   class in if + class in else      -> `Name "Bag" already defined`
    //   flat def    + class inside an if -> `Name "Bag" already defined`
    // mypy allows a CONDITIONAL FUNCTION redefinition and allows no
    // conditional class redefinition at all -- but ONLY in the order
    // "conditional first, flat second": `def in if + flat def` DOES collide
    // under real mypy, yet this rule stays silent on it too, since only the
    // FIRST occurrence's `at_flat_top_level` is ever recorded and it is
    // false. That is a deliberately over-applied allowance -- it misses a
    // real error in that one ordering rather than risk a false one, the
    // safe direction this pass exists to protect. So the collision fires
    // when either definition is a class, or when both sit flat in the
    // module body.
    // Reporting every def/def pair this recursion now reaches would be a
    // false TypeError on mypy-clean code -- the exact invariant this pass
    // exists to protect -- which is why the kind and the flatness are both
    // recorded rather than just the line.
    for_each_flat_statement(
        module.body(), /*directly_in_body=*/true,
        [this](const ast::Stmt& statement, bool at_flat_top_level) {
            const ast::Node* node = nullptr;
            std::string name;
            int line = 0;
            bool is_class = false;
            if (const auto* class_def = dynamic_cast<const ast::ClassDef*>(&statement)) {
                node = class_def;
                name = class_def->name();
                line = class_def->span().start_line;
                is_class = true;
            } else if (const auto* function_def =
                           dynamic_cast<const ast::FunctionDef*>(&statement)) {
                node = function_def;
                name = function_def->name();
                line = function_def->span().start_line;
            } else {
                return;
            }

            const auto existing = top_level_definitions_.find(name);
            if (existing == top_level_definitions_.end()) {
                top_level_definitions_.emplace(
                    name, TopLevelDefinition{line, is_class, at_flat_top_level});
                return;
            }
            if (!is_class && !existing->second.is_class &&
                !(at_flat_top_level && existing->second.at_flat_top_level)) {
                // Two functions, at least one of them conditional: mypy's
                // conditional-function-definition allowance, measured above.
                return;
            }
            report(*node, DiagnosticKind::SemanticAnalyzerTypeError,
                   "name \"" + name + "\" already defined on line " +
                       std::to_string(existing->second.line));
            collided_top_level_.insert(node);
        });
}

std::vector<Type> TypeChecker::base_types(const std::vector<ast::ExprPtr>& bases) {
    AnnotationResolver resolver(classes_, sink_);
    std::vector<Type> types;
    types.reserve(bases.size());
    for (const ast::ExprPtr& base : bases) {
        if (dynamic_cast<const ast::Attribute*>(base.get()) != nullptr) {
            // A plain dotted base (`Outer.Inner`, `mod.Thing`) is
            // deliberately NOT resolved. This is the ONE choke point for that
            // decision; the reasoning lives here rather than being repeated
            // at the validation loop.
            //
            // A dotted base WAS validated for a while, by walking the chain
            // down to its root Name and checking that root. The trouble is
            // that a root which is not a known class may still legitimately
            // hold a class object -- `h = Holder` then `class D(h.Inner):`,
            // which both oracles accept (measured: mypy --strict "Success: no
            // issues found in 1 source file"; CPython prints a D instance) --
            // and this model cannot see that, because class names are
            // deliberately never bound into ScopeStack and ClassTable is
            // keyed by class NAME, not by the values ordinary bindings hold.
            // Avoiding a false NameError there meant asking ScopeStack
            // whether the root was bound at all, which forced base validation
            // to run after the name pre-binding passes, which forced those
            // passes to recurse through control flow, which turned an
            // ordinary loop read into a false NameError on code both oracles
            // accept. Every step was a fix for a real false positive and
            // every step produced the next one, so the arm was dropped rather
            // than gated again -- and AnnotationResolver's resolve_attribute,
            // which does not know that history, must not be allowed to
            // reintroduce it one level down.
            //
            // THE TWO ACCEPTED MISSES, both measured (mypy 1.18.1, CPython
            // 3.14.2), and neither unreachable -- each is two lines to write:
            //
            //   `class D(Outer.Inner): pass` above `class Outer:` /
            //   `class Inner: pass`. mypy --strict: "Success: no issues found
            //   in 1 source file". CPython: `NameError: name 'Outer' is not
            //   defined` raised from the `class D(Outer.Inner):` statement
            //   itself, caret under `Outer` alone. One oracle rejects, so the
            //   union rule says this must not compile, and it does.
            //
            //   `class D(mod.Thing): pass` with `mod` bound nowhere. mypy
            //   --strict: `Name "mod" is not defined  [name-defined]` (plus
            //   `Class cannot subclass "Thing" (has type "Any")`). CPython:
            //   `NameError: name 'mod' is not defined`. BOTH oracles reject,
            //   and this compiler is silent.
            //
            // Both are MISSED errors, which is the safe direction of the two:
            // a program that should have been refused compiles, rather than a
            // program both oracles accept being refused. The alternatives on
            // offer were the false positives above.
            //
            // Unknown (rather than skipping the push) keeps this entry
            // aligned index-for-index with the base expression list, which
            // validate_class_bases walks in lockstep.
            types.push_back(Type::unknown());
            continue;
        }
        types.push_back(resolver.resolve(*base));
    }
    return types;
}

std::size_t TypeChecker::defaulted_param_count(const std::vector<ast::Parameter>& params) {
    std::size_t count = 0;
    for (const ast::Parameter& parameter : params) {
        if (parameter.default_value != nullptr) {
            ++count;
        }
    }
    return count;
}

std::vector<std::string> TypeChecker::param_names_of(const std::vector<ast::Parameter>& params) {
    std::vector<std::string> names;
    names.reserve(params.size());
    for (const ast::Parameter& parameter : params) {
        names.push_back(parameter.name);
    }
    return names;
}

bool TypeChecker::has_identical_signature(const Binding& existing, const Type& signature,
                                          const std::vector<std::string>& param_names) {
    // Both sides must be signatures at all, which is reached with a
    // non-Callable `existing` whenever a `def` collides with a VARIABLE
    // binding of the same name (`g: int = 1` then a conditional `def g`,
    // mypy's `Incompatible redefinition`).
    //
    // HONESTLY DEFENSIVE, not load-bearing, and measured: deleting this whole
    // guard fails NO test. The variable-collision class keeps reporting
    // without it, because same_type_up_to_union_order compares `kind` first
    // and Int is not Callable. What the guard actually buys is the
    // `signature.args.size() - 1` below, which would underflow to a huge
    // size_t for an args-empty Callable -- unreachable through
    // Type::callable, which always pushes a return type, but a silently wrong
    // answer rather than a crash if some future producer ever emits one. Kept
    // for that, and stated as defensive so a reader does not credit it with
    // the collision class it does not decide.
    if (existing.type.kind != TypeKind::Callable || signature.kind != TypeKind::Callable ||
        existing.type.args.empty() || signature.args.empty()) {
        return false;
    }
    if (!same_type_up_to_union_order(existing.type, signature)) {
        return false;
    }
    // Equal `args` sizes are guaranteed by the line above, and `args` is
    // parameters followed by the return type, so this is both sides' count.
    const std::size_t parameters = signature.args.size() - 1;
    // Names are RECORDED only when the existing binding came from a `def`,
    // which is what the length agreement tests: a `g = h` binding carries
    // h's Callable type with an empty name vector, and for a non-zero
    // parameter count that disagreement is how the two are told apart. When
    // they are not recorded the comparison falls back to types only -- see
    // Binding::param_names for the measurement that rules out the opposite
    // default.
    if (existing.param_names.size() == parameters && existing.param_names != param_names) {
        return false;
    }
    return true;
}

void TypeChecker::collect_classes(const ast::Module& module) {
    // Recursed through control flow (see for_each_flat_statement), so a
    // `class` written inside an `if`/`while`/`for` is declared exactly like a
    // flat one -- Python introduces no scope for a control-flow block, so such
    // a class binds its name in module scope. Walking only the flat list left
    // one undeclared entirely, making every use of it a false NameError on
    // code mypy accepts and CPython runs.
    for_each_flat_statement(module.body(), /*directly_in_body=*/true,
                            [this](const ast::Stmt& statement, bool) {
        if (const auto* class_def = dynamic_cast<const ast::ClassDef*>(&statement)) {
            if (collided_top_level_.count(class_def) != 0) {
                // scan_top_level_names already reported this ClassDef as a
                // redefinition; declaring it anyway would silently overwrite
                // the WINNING same-named class's ClassTable entry, since
                // declare() has no collision detection of its own.
                // collect_signatures already skips a collided FunctionDef for
                // the identical reason.
                return;
            }
            declare_class_recursive(*class_def, "", declared_classes_);
        }
    });

    // THEN -- once every class at every nesting depth is declared -- walk the
    // resolved bases for the one shape that has to be refused as a whole
    // rather than reported by the resolver: a `tuple` base.
    //
    // Base RESOLUTION itself already happened, per class, inside the loop
    // above: declare_class_recursive calls base_types while it declares, so a
    // base naming a class that is not declared YET is reported right there,
    // by AnnotationResolver, and lands here as Unknown. That timing is what
    // gives execution order for free -- see validate_class_bases.
    validate_class_bases(declared_classes_);
}

void TypeChecker::validate_class_bases(const std::vector<ClassDeclaration>& all_classes) {
    for (const ClassDeclaration& declaration : all_classes) {
        const std::vector<ast::ExprPtr>& base_exprs = declaration.node->bases();
        // One resolved Type per base expression, in the same order, by
        // construction -- both came from the SAME base_types(...) call at
        // the declaration site. Asserted rather than assumed, since a future
        // change desyncing the two would otherwise silently pair a base
        // expression with the wrong resolved Type.
        assert(base_exprs.size() == declaration.base_types.size());
        for (std::size_t index = 0;
             index < base_exprs.size() && index < declaration.base_types.size(); ++index) {
            const ast::ExprPtr& base = base_exprs[index];
            const Type& resolved_type = declaration.base_types[index];
            if (resolved_type.kind == TypeKind::Tuple) {
                // A tuple base is DEFERRED, not modelled. Measured: mypy
                // accepts `class MyPair(tuple[int, str])` and reveals
                // `MyPair()[0]` as `builtins.int`, but at runtime `MyPair()`
                // is `()` with a length of 0 and `MyPair()[0]` raises
                // IndexError -- mypy models a tuple subclass as a tuple type
                // with a nominal fallback and never checks that construction
                // produces the claimed arity. This compiler emits code that
                // has to run, so following mypy here would be a wrong-code
                // bug. NotImplementedError rather than TypeError because the
                // program may well be one mypy accepts; naming the construct
                // is the honest answer.
                report(*base, DiagnosticKind::NotImplementedError, "a tuple base class is not supported");
            }
            // AND NOTHING ELSE. Every other way a base can be wrong is
            // already reported, by AnnotationResolver, at the moment
            // base_types resolved it -- an unresolved name, a bare generic
            // missing its type parameters, a bad subscript -- and lands here
            // as Unknown. Reporting again from this loop would be a second
            // diagnostic for one root cause.
            //
            // In particular EXECUTION ORDER is now EMERGENT rather than
            // explicit, and that is the property to preserve if this loop is
            // ever touched. A base must be bound by the time the `class`
            // statement RUNS: CPython raises `NameError: name 'Parent' is not
            // defined` from `class Child(Parent):` written above
            // `class Parent:`, while mypy is order-insensitive and silent, so
            // the union rule says the program must not compile. It does not,
            // for free: declare_class_recursive resolves each class's bases
            // DURING the declaration walk, in source order, so `Parent` is
            // simply not in ClassTable yet when `Child`'s base is resolved and
            // the resolver reports it there. Measured (mypy 1.18.1, CPython
            // 3.14.2): mypy "Success: no issues found in 1 source file";
            // CPython `NameError: name 'Parent' is not defined`; this compiler
            // reports exactly one NameError, at the base expression.
            //
            // An EXPLICIT order rule -- comparing the base's declaration line
            // against the subclass's -- lived here and was deleted: with the
            // resolver already catching the real case, the only inputs still
            // reaching it were builtin SHADOWINGS (`class Sub(Exception):`
            // above a later `class Exception:`, and the `int` equivalent),
            // where the base resolves through the SEEDED builtin entry that
            // the class statement below merely shadows afterwards. Measured:
            // both are mypy "Success" and both run clean under CPython, so the
            // rule's whole remaining output was a false NameError.
        }
    }
}

std::string TypeChecker::declare_isolated_class(const ast::ClassDef& node,
                                                const std::string& qualified_name) {
    std::vector<Type> resolved_bases = base_types(node.bases());
    classes_.declare(qualified_name, resolved_bases);
    std::vector<ClassDeclaration> all_classes{
        ClassDeclaration{&node, qualified_name, std::move(resolved_bases)}};
    for_each_flat_statement(node.body(), /*directly_in_body=*/true,
                            [&](const ast::Stmt& statement, bool) {
        if (const auto* nested = dynamic_cast<const ast::ClassDef*>(&statement)) {
            declare_class_recursive(*nested, qualified_name, all_classes);
        }
    });
    // A function body runs at CALL time, which is after every module-level
    // `class` statement below the def has already executed, so a
    // function-local class may legitimately name a module-level base written
    // BELOW its own def. Nothing here compares source lines, and that is what
    // keeps `def f(): class Sub(P): ...` above `class P:` clean -- measured,
    // both oracles accept it (mypy: Success; CPython: runs). The base_types
    // call above is what makes that work: it runs during the ORDINARY walk,
    // long after Phase 1 declared every module-level class, so `P` resolves.
    //
    // The in-function REVERSE order -- `class Sub(Local):` above
    // `class Local:` inside the SAME def -- is still caught, and for the same
    // emergent reason as the module-level case: a function-local class is
    // declared under an isolated qualified name and reached only through a
    // scope-limited alias that visit(ClassDef) installs when the walk
    // reaches its own statement, so when Sub's base is RESOLVED the name
    // `Local` is not a known class yet and AnnotationResolver reports it.
    // Whether that report is correct depends on whether `f` is ever CALLED,
    // and this check cannot see that far: measured, once `f()` runs, CPython
    // rejects it (UnboundLocalError: cannot access local variable 'Local'
    // where it is not associated with a value -- a NameError subclass) while
    // mypy still says Success, so under the union rule cythonpp reporting
    // NameError here is correct. But with `f` never called, the body never
    // runs, so CPython exits clean with no output and mypy still says
    // Success -- both oracles accept, and cythonpp reports the same
    // NameError regardless, over-firing on that program. This is the same
    // control-flow-insensitivity as the loop-reentry case recorded as a
    // known-wrong test below: the check has no way to tell "will run" from
    // "might never run".
    validate_class_bases(all_classes);
    return qualified_name;
}

void TypeChecker::declare_class_recursive(const ast::ClassDef& class_def,
                                          const std::string& qualified_prefix,
                                          std::vector<ClassDeclaration>& all_classes) {
    const std::string qualified_name =
        qualified_prefix.empty() ? class_def.name() : qualified_prefix + "." + class_def.name();
    std::vector<Type> resolved_bases = base_types(class_def.bases());
    classes_.declare(qualified_name, resolved_bases);
    all_classes.push_back(ClassDeclaration{&class_def, qualified_name, std::move(resolved_bases)});

    // A NESTED ClassDef (e.g. Inner inside Outer's body) is declared right
    // here, under ITS OWN qualified name -- "Outer.Inner" -- rather than
    // waiting for Phase 3's ordinary walk to reach it: an annotation
    // resolved in Phase 2 (`x: Outer.Inner`) runs BEFORE Phase 3 ever visits
    // Outer's ClassDef node, so without this recursion "Outer.Inner" would
    // not exist in ClassTable yet and the annotation would report a false
    // NameError.
    for_each_flat_statement(class_def.body(), /*directly_in_body=*/true,
                            [&](const ast::Stmt& statement, bool) {
        if (const auto* nested = dynamic_cast<const ast::ClassDef*>(&statement)) {
            declare_class_recursive(*nested, qualified_name, all_classes);
        }
    });
}

void TypeChecker::collect_signatures(const ast::Module& module) {
    // Recursed through control flow (see for_each_flat_statement), so a `def`
    // written inside an `if`/`while`/`for` is bound in module scope exactly
    // like a flat one -- Python introduces no scope for a control-flow block,
    // and a conditional def was previously never bound at all (a def's own
    // name is otherwise bound only by its own visit(FunctionDef), which binds
    // it only when the current scope is Function -- Module, at this point),
    // making every call to one a false NameError on code mypy accepts and
    // CPython runs.
    //
    // ONE ORDERED LOOP, not two passes: the bind check below relies on an
    // AnnAssign having bound first when it textually appears first, and
    // splitting into a recursed def pass plus a flat AnnAssign pass would
    // reorder every def ahead of every AnnAssign, breaking that.
    //
    // The AnnAssign arm is recursed too, and unconditionally: a conditional
    // annotated assignment (`if FLAG: y: int = 5`) is bound here exactly like
    // a flat one, at its own line, so a read above it says "used before
    // definition" instead of falling through to "not defined" (matching mypy),
    // and a later same-name definition still reports against the right
    // statement.
    // Names this pass has bound FROM A FunctionDef, so a later failed bind
    // can tell a def-vs-def collision (mypy allows it when either side is
    // conditional) apart from a def colliding with a variable binding of the
    // same name (mypy always reports that one, conditional or not) -- a
    // `Binding` cannot make that distinction on its own, since an annotated
    // assignment and a def both bind with `annotated = true`, and inferring
    // it from the existing binding's type would also wrongly suppress a
    // genuine `f: Callable[[], int] = ...` followed by a conditional `def f`.
    // Local to this pass rather than a `Binding` field: a field would have to
    // be threaded through every other bind site in this file for the benefit
    // of this one caller.
    std::set<std::string> def_bound_names;
    for_each_flat_statement(
        module.body(), /*directly_in_body=*/true,
        [this, &def_bound_names](const ast::Stmt& statement, bool at_flat_top_level) {
            if (const auto* function_def = dynamic_cast<const ast::FunctionDef*>(&statement)) {
                if (collided_top_level_.count(function_def) != 0) {
                    return; // scan_top_level_names already reported this.
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
                // Cached BEFORE the Binding below moves from it, so
                // visit(FunctionDef) can reuse this exact resolution rather
                // than calling AnnotationResolver a second time on the same
                // annotations (see top_level_signatures_'s own comment).
                // MUST happen even for a conditional def, and BEFORE the
                // early return below -- otherwise visit(FunctionDef) finds no
                // cached entry, re-resolves the same annotations itself, and
                // double-reports a bad one.
                const Type signature_type = Type::callable(
                    params, return_type, defaulted_param_count(function_def->params()));
                top_level_signatures_.emplace(function_def, signature_type);
                // The bool `bind` returns MUST be
                // checked -- `bind` itself has no sink and never reported
                // anything on its own, contrary to what the previous comment
                // here claimed. A def/def or def/class collision never
                // reaches this line at all: scan_top_level_names already
                // caught and reported both (it compares EVERY top-level
                // ClassDef/FunctionDef name against every other), and the
                // early return above already skipped the losing def. The ONE
                // collision that reaches `bind` here is an AnnAssign-then-def
                // collision -- the AnnAssign bound first, earlier in THIS
                // same ordered loop, and scan_top_level_names never tracks
                // AnnAssign names at all -- so this check exists for that
                // case specifically. The reverse, def-then-AnnAssign, is
                // instead caught by bind_annotation's own bool check below,
                // since by the time that AnnAssign runs the def already
                // occupies the name.
                const std::vector<std::string> param_names =
                    param_names_of(function_def->params());
                Binding signature{signature_type, function_def->span().start_line,
                                  /*annotated=*/true};
                // Assigned rather than passed positionally, so the four
                // members above keep meaning what they say -- see
                // Binding::param_names' own comment.
                signature.param_names = param_names;
                if (!scopes_.bind(function_def->name(), signature)) {
                    // Suppress only a def colliding with an EARLIER def, and
                    // only when at least one side is conditional -- two FLAT
                    // defs never reach this line at all, since
                    // scan_top_level_names already caught and reported that
                    // pair and the early return above already skipped the
                    // losing one, so def_bound_names.count(...) here is only
                    // ever consulted when at_flat_top_level is false for one
                    // of the two. Measured against mypy 1.18.1: two defs of
                    // one name in an if/else, and two in the same block, are
                    // BOTH `Success` -- mypy allows a conditional function
                    // redefinition. So reporting here would be a false
                    // TypeError on code mypy accepts, which is the one thing
                    // this pass must never do. The first binding wins and
                    // this one is dropped silently.
                    //
                    // The SIGNATURE-IDENTITY half is the same second
                    // condition visit(FunctionDef)'s own conditional
                    // allowance carries, so the rule is one rule at both
                    // sites. mypy's allowance is exactly "identical
                    // signatures"; measured 2026-09-11 at MODULE scope,
                    // `if C: def g() -> int ... else: def g(a: int) -> str`
                    // draws `All conditional function variants must have
                    // identical signatures  [misc]`, and so does the
                    // flat-then-conditional ordering of the same pair. Both
                    // were silently accepted here before the identity test.
                    //
                    // has_identical_signature, NOT `==`: `==` is
                    // union-order-sensitive and carries no parameter names,
                    // so it was wrong in both directions at once -- measured
                    // at this exact site, `if c: def g(a: int | str)` /
                    // `else: def g(a: str | int)` at module scope drew a
                    // false TypeError, and the same pair spelled
                    // `def g(a: int)` / `def g(b: int)` was silently accepted
                    // where mypy reports. See the predicate's own comment.
                    //
                    // A def colliding with a VARIABLE binding (annotated or
                    // not) is a different collision class entirely -- mypy
                    // reports it regardless of either side's conditionality
                    // (measured: `Incompatible redefinition`) -- so that case
                    // must always fall through to the report below. It
                    // already does, twice over: def_bound_names holds only
                    // names bound FROM a FunctionDef, and a variable's type
                    // is not a Callable, which has_identical_signature
                    // rejects outright.
                    const Resolution bound = scopes_.resolve(function_def->name());
                    if (!at_flat_top_level && def_bound_names.count(function_def->name()) != 0 &&
                        has_identical_signature(*bound.binding, signature_type, param_names)) {
                        return;
                    }
                    report(*function_def, DiagnosticKind::SemanticAnalyzerTypeError,
                          "name \"" + function_def->name() + "\" already defined on line " +
                              std::to_string(bound.binding->declared_line));
                } else {
                    def_bound_names.insert(function_def->name());
                }
            } else if (const auto* ann_assign = dynamic_cast<const ast::AnnAssign*>(&statement)) {
                if (const auto* target_name = dynamic_cast<const ast::Name*>(&ann_assign->target())) {
                    module_level_annotations_[ann_assign] = bind_annotation(
                        *target_name, ann_assign->annotation(), ann_assign->span().start_line);
                }
            }
        });
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
    // Routed through for_each_own_scope_binding -- shared with
    // receiver_rebound_in_own_scope -- so this placeholder pass and that
    // shadow check cannot recognise different binding forms. Closes a
    // measured gap (2026-09-13): the pre-refactor version only covered a
    // top-level Assign/AnnAssign/nested-def, never recursed into If/While/For
    // bodies, and never covered a `for` target at all, so
    //   def m() -> None:
    //       def inner() -> None:
    //           self.q = 1
    //           for self in [1, 2]:
    //               pass
    //       inner()
    // (with `self` a bound name in an ENCLOSING scope, so the read did not
    // simply fall through to "not defined") was silently accepted --
    // mypy reports `Name "self" is used before definition  [used-before-def]`
    // at the store, and CPython raises the matching UnboundLocalError -- both
    // oracles reject a program this compiler accepted.
    for_each_own_scope_binding(body, [this](const std::string& name, int line,
                                           OwnScopeBindingKind kind) {
        if (kind == OwnScopeBindingKind::NestedClass) {
            // Deliberately UNBOUND, not merely unhandled. A nested class's
            // own name is a genuine same-scope binding form too (measured:
            // `class Local: pass` above `def f(): print(Local); class
            // Local: pass` is mypy `used-before-def` and CPython
            // `UnboundLocalError`, and this compiler is silent on it), but
            // placeholder-binding it here would break every "is_class(name)
            // implies scopes_.resolve(name) is null" carve-out this compiler
            // already depends on elsewhere (type_of_name's bare-class-value
            // branch, ExpressionTyper::class_object_receiver, and the bare
            // `C()` constructor branch in expression_typer_calls.cpp) the
            // moment the class's own definition is reached -- ScopeStack has
            // no removal primitive to restore the invariant afterward, and
            // filling the placeholder with the class's constructor type
            // (the one value that would keep those carve-outs' ANSWER
            // correct) still leaves them looking at a non-null binding, which
            // is the fact they key on, not the type. Left open; see the
            // dated entry in CLAUDE.md.
            return;
        }
        if (!scopes_.bound_in_current_scope(name)) {
            scopes_.bind(name, Binding{Type::unknown(), line, /*annotated=*/false});
        }
    });
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
            // Routed through is_unfilled_placeholder (rather
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
        report(target, DiagnosticKind::SemanticAnalyzerTypeError,
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
            report(*name, DiagnosticKind::TypeCheckerTypeError,
                   "need type annotation for \"" + name->identifier() + "\"");
        }
        assign_name(*name, value_type, line);
        if (is_new_definition && scopes_.current_kind() == ScopeKind::Class) {
            // A plain class-body Assign (`class D:
            // x = 5`) never declared an instance attribute at all before
            // this -- only the AnnAssign path did. Declared on the FIRST
            // real assignment only (is_new_definition, already computed
            // above for the bare-empty-container check), matching every
            // other "first assignment is sticky" rule in this checker --
            // pre_collect_class_body may already have placeholder-declared
            // this exact name as Unknown at this exact line (so a method
            // ABOVE this statement reading self.x already sees it exists).
            //
            // An earlier version of this arm had NO guard at all --
            // unconditionally overwriting classes_.declare_member,
            // even when a member ALREADY exists under a DIFFERENT line (a
            // genuine earlier self.x = ... assignment in a method ABOVE this
            // statement), silently RE-TYPING the attribute with zero
            // diagnostics. That is the exact defect already fixed for the
            // AnnAssign sibling and assign_attribute's own self.x path; this
            // was the one place the guard was missed.
            // Routed through the SAME member_declared_line line-equality
            // disambiguation assign_attribute already uses: a member at
            // THIS exact line is this statement's own placeholder (fill in
            // the real type -- no comparison, nothing real to compare
            // against yet); anything else is a genuine earlier declaration,
            // compared rather than silently overwritten.
            //
            // WHICH CLASS the earlier declaration lives on decides the rule
            // -- the same two-rule split as the class-body AnnAssign sibling
            // and the `self.x: T = ...` branch, gated the same way, on
            // own_member_type rather than the chain-walking member_type.
            // Both rules verified against mypy 1.18.1, including reveal_type
            // on the resulting attribute:
            //
            //   SAME CLASS (own_member_type finds it, at a line other than
            //     this statement's): this class-body assignment's inferred
            //     type is the DECLARED type of the attribute for the whole
            //     class body, and the earlier `self.x = ...` value is what
            //     gets checked AGAINST it -- exactly the rule the class-body
            //     ANNOTATION follows. So `self.x = True` above `x = 5` is
            //     clean (bool widens into the declared int), `self.x = 5`
            //     above `x = 1.5` is clean (int widens into float), and only
            //     a genuine mismatch such as `self.x = 5` above `x = "s"`
            //     reports -- with mypy's own exact wording, `expression has
            //     type "int", variable has type "str"`. The LINE still
            //     differs from mypy's (we report at this class-body
            //     statement, mypy at the self.x assignment), a recorded
            //     residual this does not close. Running that comparison the
            //     other way round alone made two mypy-clean programs false
            //     TypeErrors. An earlier CLASS-BODY declaration of the same
            //     name never reaches here at all: it binds the name in the
            //     class SCOPE, so is_new_definition above is false and this
            //     whole arm is skipped in favour of assign_name's ordinary
            //     compare -- which is what keeps `x: float` above `x = 5`
            //     (mypy-clean, and float stays the attribute's type) clean.
            //
            //   INHERITED (a base declares it, own_member_type does not): a
            //     narrowing override, accepted, and it becomes THIS class's
            //     own declared type -- `class Base: v: object` / `class
            //     Child(Base): v = 1` is mypy-clean with reveal_type(self.v)
            //     "builtins.int" in Child, so the inferred type must be
            //     INSTALLED here or Child's own `self.v + 1` is a false
            //     TypeError. Widening is the error, with the arguments the
            //     other way round from the same-class rule: mypy says
            //     `expression has type "str", base class "Base" defined the
            //     type as "int"`, so the VALUE is the expression and the
            //     inherited declaration is the variable.
            const bool already_method =
                classes_.method_type(current_class_qualified_name_, name->identifier()).has_value();
            const std::optional<Type> own_existing =
                classes_.own_member_type(current_class_qualified_name_, name->identifier());
            const std::optional<int> existing_line =
                classes_.member_declared_line(current_class_qualified_name_, name->identifier());
            const bool is_brand_new = !already_method && !existing_line.has_value();
            // A placeholder is by construction one this class declared on
            // ITSELF, so this asks own_member_type as well as the line: an
            // inherited declaration must never be mistaken for this
            // statement's own placeholder, however the lines fall.
            const bool is_own_placeholder = !already_method && own_existing.has_value() &&
                                            existing_line.has_value() && *existing_line == line;
            if (is_brand_new || is_own_placeholder) {
                classes_.declare_member(current_class_qualified_name_, name->identifier(), value_type,
                                        line);
            } else if (!already_method && own_existing.has_value()) {
                // Unknown on either side leaves nothing to compare: silent,
                // and the value's type still installs below (Unknown is
                // absorbing, so the worst case is a later missed error).
                if (value_type.kind != TypeKind::Unknown &&
                    own_existing->kind != TypeKind::Unknown &&
                    !is_subtype(*own_existing, value_type, &classes_)) {
                    report_incompatible_assignment(*name, *own_existing, value_type, "variable");
                }
                // The class-body assignment's type WINS, reported or not:
                // mypy treats it as the attribute's declared type for the
                // whole class body (reveal_type(C().x) is "str" for
                // `self.x = 5` above `x = "s"`, and "float" for `self.x = 5`
                // above `x = 1.5`), so leaving the earlier self.x's inferred
                // type in place would keep every LATER read of the attribute
                // resolving to the wrong type. The line moves with it, so a
                // subsequent statement's own placeholder disambiguation
                // still compares against this declaration rather than
                // mistaking it for its own.
                classes_.declare_member(current_class_qualified_name_, name->identifier(),
                                        value_type, line);
            } else if (!already_method && existing_line.has_value()) {
                // Inherited only: own_member_type missed and the chain walk
                // hit. Unknown on either side is silent for the same reason
                // as above, and the value's type is installed over the
                // inherited one regardless -- mypy treats this class's own
                // declaration as authoritative, so checking a later write
                // against the base's type instead is how a false TypeError
                // gets made.
                const Type inherited_type =
                    *classes_.member_type(current_class_qualified_name_, name->identifier());
                if (value_type.kind != TypeKind::Unknown &&
                    inherited_type.kind != TypeKind::Unknown &&
                    !is_subtype(value_type, inherited_type, &classes_)) {
                    report_incompatible_assignment(*name, value_type, inherited_type, "variable");
                }
                classes_.declare_member(current_class_qualified_name_, name->identifier(),
                                        value_type, line);
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
        // pre_bind_assignment_targets/pre_bind_function_body (or is
        // re-visiting its own earlier tuple element within the same
        // statement) -- this IS the first real assignment, so fill it in
        // rather than compare against the Unknown placeholder.
        //
        // order_exempt is threaded through here, not hardcoded false: a `for`
        // target's placeholder (pre_bind_function_body, since 2026-09-13)
        // shares this exact fill path, and its real bind always passes
        // order_exempt=true (see visit(For)) -- a one-line suite reading the
        // target right back (`for i in range(3): print(i)`, now reachable
        // through this branch once the target is pre-bound) would otherwise
        // lose the flag and misfire a false "used before definition" exactly
        // like the bug order_exempt exists to prevent. An ordinary Assign's
        // own placeholder-fill call never passes true, so this is a no-op for
        // every pre-existing caller.
        scopes_.rebind(target.identifier(), Binding{value_type, line, /*annotated=*/false, order_exempt});
        return;
    }
    // A genuine reassignment (including a one-line def's parameter, whose
    // declared_line equals this very statement's line but which is
    // order_exempt -- see is_unfilled_placeholder -- so it never takes the
    // branch above): the FIRST assignment's inferred type is sticky, so this
    // is a compatibility check only, never a rebind. This is what makes
    // `def f(x: int) -> None: x = "s"` a reported incompatible assignment
    // instead of a silent rebind that discards the parameter's annotation.
    if (value_type.kind != TypeKind::Unknown && existing.binding->type.kind != TypeKind::Unknown &&
        !is_subtype(value_type, existing.binding->type, &classes_)) {
        report_incompatible_assignment(target, value_type, existing.binding->type, "variable");
        // The entry is left UNTOUCHED: the declared type is a permanent
        // ceiling, so a rejected assignment must not narrow to a type that
        // ceiling forbids.
        return;
    }
    // Compatible with the declared ceiling, so this becomes the path's
    // current type. kill() first, so any narrowing on a path BENEATH this one
    // goes with it -- `x = C()` invalidates whatever was known about `x.y`.
    narrowings_.kill(target.identifier());
    narrowings_.set(target.identifier(), value_type);
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

std::optional<Type> TypeChecker::self_attribute_receiver_type(const ast::Attribute& target) const {
    const auto* receiver = dynamic_cast<const ast::Name*>(&target.value());
    if (receiver == nullptr || current_class_qualified_name_.empty()) {
        return std::nullopt;
    }
    // Resolved by the receiver's OWN spelling, not the literal "self": mypy
    // keys on which BINDING the receiver resolves to, and a method's first
    // parameter may be named anything. Measured 2026-09-12 --
    // `def m(this) -> None: this.q = 1` with a reader above is mypy Success
    // and CPython prints 1, where keying on "self" reported twice.
    const Resolution self_resolution = scopes_.resolve(receiver->identifier());
    if (self_resolution.binding == nullptr ||
        self_resolution.binding->type.kind != TypeKind::Class ||
        self_resolution.binding->type.name != current_class_qualified_name_) {
        return std::nullopt;
    }
    // The type check above is necessary but NOT sufficient: a nested
    // `def inner(self: Bag)` inside a Bag method binds `self` to exactly
    // Class("Bag") and passes it, so `self.q = 1` there used to declare "q"
    // on Bag and make a later read of it come out clean where mypy reports an
    // attribute miss at both the store and the read. The binding must ALSO be
    // a method's own first parameter -- a fact about the BINDING, deliberately
    // not about the innermost function, since mypy attributes a store to the
    // method's self through any number of capturing closures (see
    // Binding::method_self for both measurements).
    //
    // collect_self_attribute_placeholders now DOES descend into a nested def,
    // matching this real walk: a reader ABOVE a closure that captures the
    // method's own receiver must see the attribute the closure declares,
    // exactly as method_self lets the real walk attribute
    // the store through any number of scopes. The pre-pass stops descending
    // only at a def that REBINDS the receiver name (a real shadow) or at a
    // nested class (a different `self` entirely) -- see
    // collect_self_attribute_placeholders' own comment.
    if (!self_resolution.binding->method_self) {
        return std::nullopt;
    }
    return self_resolution.binding->type;
}

TypeChecker::SelfMemberState TypeChecker::self_member_state(const std::string& attribute,
                                                            int line) const {
    if (classes_.method_type(current_class_qualified_name_, attribute).has_value()) {
        return SelfMemberState::ExistingDeclaration;
    }
    const std::optional<int> existing_line =
        classes_.member_declared_line(current_class_qualified_name_, attribute);
    if (!existing_line.has_value()) {
        // No member and no method at all -- an attribute
        // pre_collect_class_body's scan could not see, e.g. one first
        // assigned inside a nested def's own body.
        return SelfMemberState::BrandNew;
    }
    return *existing_line == line ? SelfMemberState::OwnPlaceholder
                                  : SelfMemberState::ExistingDeclaration;
}

void TypeChecker::assign_attribute(const ast::Attribute& target, const ast::Expr& value) {
    // THE TRAP's escape hatch (Task 19): `self.x = ...` inside a method
    // declares a NEW instance attribute the first time it is seen, checked
    // BEFORE the ordinary read-then-compare path below -- which would
    // otherwise call type_of_attribute on a member that does not exist YET
    // and report a false attr-defined TypeError. The guard itself lives in
    // self_attribute_receiver_type and the three-way line disambiguation in
    // self_member_state, both shared verbatim with visit(AnnAssign)'s
    // `self.x: T = ...` branch -- see those two for the full mechanism.
    if (const std::optional<Type> self_type = self_attribute_receiver_type(target)) {
        const SelfMemberState state = self_member_state(target.attribute(), target.span().start_line);
        if (state == SelfMemberState::BrandNew || state == SelfMemberState::OwnPlaceholder) {
            // Either the FIRST self.x = ... TypeChecker's own visitation has
            // reached for this name (in THIS class; a base's member/method of
            // the same name is ExistingDeclaration instead and falls through
            // to the ordinary path), or this exact statement's own
            // placeholder from pre_collect_class_body -- infer the type from
            // the value, exactly like an ordinary Name assignment, and
            // declare it (overwriting the Unknown placeholder, in the latter
            // case, with the real one). No comparison: there is nothing REAL
            // yet to compare against either way.
            const Type value_type = typer_.type_of(value, Type::unknown());
            classes_.declare_member(current_class_qualified_name_, target.attribute(), value_type,
                                    target.span().start_line);
            // type_of_attribute never ran for `target`, so its TypeMap
            // entries would otherwise be missing -- recorded by hand,
            // matching type_of_attribute's own class-object-receiver
            // branch (expression_typer.cpp), which does the same for the
            // same reason.
            types_.insert(&target, value_type);
            types_.insert(&target.value(), *self_type);
            // This IS the declared type, so no entry is needed -- but a
            // narrowing on a path beneath this one still has to go.
            if (const std::optional<NarrowedPath> path = narrowing_path_of(target)) {
                narrowings_.kill(*path);
            }
            return;
        }
    }

    // Typed for its side effects -- the TypeMap entries and the attr-defined
    // report -- but NOT used as the comparison target: it now carries the
    // NARROWED type, and comparing against that would make narrowing restrict
    // what may be assigned next. The declared type is the ceiling, always.
    //
    // Reuses type_of_attribute entirely, which already reports attr-defined
    // ("\"C\" has no attribute \"x\"") for a name the class never declares --
    // the attribute set is closed at the class definition, so assigning a
    // NEW attribute from outside the class is exactly that error. Also the
    // path a SECOND, conflicting self.x assignment falls through to (the
    // member now exists, from the first assignment above), matching
    // assign_name's own "first assignment's type is sticky" rule -- no join,
    // no union, just a compatibility check against the already-declared type.
    //
    // Label corrected from "target" to "variable" --
    // verified against mypy 1.18.1 (both a same-class conflicting self.x
    // assignment and an outside-the-class `c.x = ...` reassignment) that an
    // attribute target's own incompatible-assignment message always reads
    // "variable has type ...", never "target has type ...", exactly like an
    // ordinary Name target's (assign_name already used "variable"; this was
    // the one call site left saying something else with no test pinning it).
    const Type value_type = typer_.type_of(value, Type::unknown());
    const Type narrowed_or_declared = typer_.type_of(target, Type::unknown());
    const std::optional<NarrowedPath> path = narrowing_path_of(target);
    const Type ceiling = path.has_value()
                             ? declared_type_of_path(*path).value_or(narrowed_or_declared)
                             : narrowed_or_declared;
    if (value_type.kind != TypeKind::Unknown && ceiling.kind != TypeKind::Unknown &&
        !is_subtype(value_type, ceiling, &classes_)) {
        report_incompatible_assignment(target, value_type, ceiling, "variable");
        return;
    }
    if (path.has_value()) {
        narrowings_.kill(*path);
        narrowings_.set(*path, value_type);
    }
}

std::optional<Type> TypeChecker::declared_type_of_path(const NarrowedPath& path) const {
    std::size_t start = 0;
    const std::size_t first_dot = path.find('.');
    const std::string root = path.substr(0, first_dot);
    const Resolution resolution = scopes_.resolve(root);
    if (resolution.binding == nullptr) {
        return std::nullopt;
    }
    Type current = resolution.binding->type;
    start = first_dot;
    while (start != std::string::npos) {
        const std::size_t next = path.find('.', start + 1);
        const std::string link = next == std::string::npos
                                     ? path.substr(start + 1)
                                     : path.substr(start + 1, next - start - 1);
        if (current.kind != TypeKind::Class) {
            return std::nullopt;
        }
        const std::optional<Type> member = classes_.member_type(current.name, link);
        if (!member.has_value()) {
            return std::nullopt;
        }
        current = *member;
        start = next;
    }
    return current;
}

void TypeChecker::redeclare_narrowing(const ast::Expr& target,
                                      const std::optional<Type>& narrowed) {
    const std::optional<NarrowedPath> path = narrowing_path_of(target);
    if (!path.has_value()) {
        return;
    }
    narrowings_.kill(*path);
    if (narrowed.has_value()) {
        narrowings_.set(*path, *narrowed);
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
        // A class-body AnnAssign, whether flat or NESTED inside an
        // if/while/for (pre_collect_class_body recurses control flow just as
        // this walk does), was already resolved once by
        // pre_collect_class_body's own eager pass -- reuse that cached Type
        // via bind_resolved_annotation (the scope-bind half of
        // bind_annotation) rather than invoking AnnotationResolver a second
        // time. The cache is what keeps a bad annotation from being reported
        // TWICE for the nested case: without it, this fallback would call
        // the un-cached bind_annotation and re-resolve (and re-report) the
        // same annotation pre_collect_class_body's pass already reported.
        // Verified: `class C:` / `    if FLAG:` / `        x: Nope = 1`
        // (with FLAG bound) draws exactly one NameError for "Nope", not two.
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
            // Guarded rather than declared unconditionally -- without a
            // guard, an EARLIER self.x = ... assignment (in a method
            // occurring ABOVE this annotation in the class body) is silently
            // re-typed with zero diagnostics, since declare_member has no
            // collision detection of its own.
            //
            // WHICH CLASS the earlier declaration lives on decides the rule,
            // exactly as it does in the `self.x: T = ...` branch further
            // down -- so the gate is own_member_type (a direct-entry lookup,
            // keyed exactly as declare_member keys it) and NOT the
            // chain-walking member_type, which reports a hit for an
            // inherited declaration and an own one alike. A single
            // comparison covering both was a false TypeError on one of them
            // whichever way round it was written: with the same-class
            // direction below, `class Base: v: object` / `class Child(Base):
            // v: int` -- mypy-clean narrowing -- was reported, while the
            // widening mypy DOES reject passed silently.
            //
            // The two rules, both measured against mypy 1.18.1, and NOT the
            // two rules the `self.x: T` branch uses -- its same-class half is
            // the OPPOSITE of this one, because a class-body declaration
            // outranks a method-level one whatever the textual order, and
            // here the class-body statement IS the declaration:
            //
            //   SAME CLASS (own_member_type finds it, at a line other than
            //     this statement's own -- see the line test below): this
            //     annotation is the declared type and the earlier
            //     declaration's type is the expression checked AGAINST it.
            //     `FLAG = True / class C: def m(self): self.x = True / if
            //     FLAG: x: int` is clean, and reveal_type(self.x) in a
            //     second method is "builtins.int". That is why the
            //     comparison keeps the existing declaration as the
            //     expression and installs the annotation below.
            //
            //     The near-miss worth naming, because it looks like this arm
            //     and is not: `self.x = 5` above `if FLAG: x: str`. mypy
            //     reports `Incompatible types in assignment (expression has
            //     type "int", variable has type "str")` at the SELF.X line,
            //     and so do we -- same line, same wording, but from
            //     assign_attribute, not from here. The eager annotation
            //     sub-pass installs `x: str` at the annotation's own line
            //     before any method body is walked, so this statement's own
            //     line test passes and the same-class arm is skipped.
            //
            //   INHERITED (a base declares it, own_member_type does not): a
            //     narrowing override, accepted, and it really does become
            //     THIS class's own declared type -- `class Base: v: object` /
            //     `class Child(Base): v: int` reveals "builtins.int" in every
            //     Child method and "builtins.object" in Base, and a third
            //     level (`class Leaf(Mid): v: bool`) reads as bool/int/object
            //     per class. So the annotation must be INSTALLED on the
            //     current class, or Child's own mypy-clean `self.v + 1` is a
            //     false TypeError. Widening is the error, with the arguments
            //     the other way round: mypy says `expression has type
            //     "object", base class "Base" defined the type as "int"`, so
            //     the ANNOTATION is the expression and the inherited
            //     declaration is the variable. (Same for a nested and for a
            //     function-local class -- own_member_type does no
            //     canonicalisation, and both are declared under the exact
            //     qualified name this call passes it.)
            const std::optional<Type> own_existing = classes_.own_member_type(
                current_class_qualified_name_, target_name->identifier());
            // own_existing alone no longer tells "this class already had a
            // REAL declaration" apart from "the eager member-collection
            // phase (visit(Module)) already installed THIS EXACT statement's
            // own entry" -- pre_collect_class_body's annotation sub-pass now
            // installs every direct class-body annotation's own entry up
            // front (own_member_type gate, see its own comment), so by the
            // time this real walk reaches the SAME node, own_existing always
            // hits. The declared LINE is the disambiguator, exactly like
            // is_unfilled_placeholder's ScopeStack analogue: it is THIS
            // statement's own install only when the line matches, and a
            // genuinely earlier same-class declaration otherwise. What that
            // second case can actually BE is narrow, and worth stating so
            // the arm below is not mistaken for the common path: every
            // ordinary shape that looks like it lands on the other side of
            // the test. A second class-body annotation of a name is a
            // redefinition, which skips this whole block via
            // info.redefinition. An earlier `self.x = ...` in a method above
            // this annotation loses the race outright -- the eager
            // annotation sub-pass runs over the whole class body BEFORE any
            // method is scanned, so the annotation owns its own line and the
            // method's assignment is checked against it by assign_attribute
            // instead. What is left is a class body pre-collected twice
            // under one qualified name: two same-named nested classes in one
            // outer class, which ClassTable keys identically and whose
            // duplicate-name error this model does not report.
            //
            // own_member_declared_line, not the canonicalising chain-walking
            // member_declared_line: the value being disambiguated came out
            // of own_member_type, so the line has to come out of the SAME
            // member map under the SAME key, or the two can describe
            // different classes' entries.
            const std::optional<int> own_declared_line = classes_.own_member_declared_line(
                current_class_qualified_name_, target_name->identifier());
            const bool own_is_this_statement =
                own_existing.has_value() && own_declared_line.has_value() &&
                *own_declared_line == node.span().start_line;
            const bool already_method =
                classes_.method_type(current_class_qualified_name_, target_name->identifier())
                    .has_value();
            if (own_existing.has_value() && !own_is_this_statement) {
                // A genuinely earlier SAME-CLASS declaration -- this
                // annotation is the declared type and the earlier
                // declaration's type is the expression checked AGAINST it,
                // per the SAME CLASS rule above.
                //
                // Reached only by the narrow case that comment names: two
                // same-named nested classes in one outer class, both keyed
                // under the same qualified name, so the second body's
                // annotation finds the FIRST body's install at a different
                // line. Every other candidate was probed and lands
                // elsewhere. mypy rejects that program for the duplicate
                // name (`Name "Inner" already defined on line N`), an error
                // this model does not report at all, so what this arm
                // contributes there is a second, different diagnostic on an
                // already-rejected program -- never a report on anything
                // mypy accepts.
                //
                // Unknown on either side means there is nothing to compare:
                // no report, and the annotation still installs below -- an
                // Unknown existing declaration (an earlier
                // `self.x = <unmodellable>`) is not a declaration worth
                // keeping, and an Unknown annotation replacing a known one
                // only silences later reads, a missed error rather than a
                // false one.
                if (info.type.kind != TypeKind::Unknown &&
                    own_existing->kind != TypeKind::Unknown &&
                    !is_subtype(*own_existing, info.type, &classes_)) {
                    report_incompatible_assignment(node, *own_existing, info.type, "variable");
                }
                classes_.declare_member(current_class_qualified_name_, target_name->identifier(),
                                        info.type, node.span().start_line);
            } else {
                // Either BRAND NEW, or exactly what the eager
                // member-collection phase installed for THIS statement --
                // either way, this class's own entry (if any) carries
                // nothing to compare against, so check the INHERITED chain
                // directly (bypassing this class's own entry entirely,
                // rather than own_member_type/member_type, which would just
                // find this same statement's own install again). `class
                // Base: v: object` / `class Child(Base): v: int` reveals
                // "builtins.int" in every Child method and "builtins.object"
                // in Base, and a third level (`class Leaf(Mid): v: bool`)
                // reads as bool/int/object per class -- so the annotation
                // must be INSTALLED on the current class, or Child's own
                // mypy-clean `self.v + 1` is a false TypeError. Widening IS
                // reported, with the arguments the other way round: mypy
                // says `expression has type "object", base class "Base"
                // defined the type as "int"`, so the annotation is the
                // `expression` and the inherited declaration is the
                // `variable`.
                const std::optional<Type> inherited_existing = classes_.inherited_member_type(
                    current_class_qualified_name_, target_name->identifier());
                if (inherited_existing.has_value()) {
                    // Unknown on either side, same handling and same
                    // reasoning as the same-class arm above, with one extra
                    // consequence worth naming: an Unknown annotation is
                    // installed OVER a known inherited type rather than
                    // leaving the base's in place, because mypy treats this
                    // class's own declaration as authoritative -- keeping the
                    // base's type would check a later write against a type
                    // the attribute no longer has, which is how a false
                    // TypeError gets made.
                    if (info.type.kind != TypeKind::Unknown &&
                        inherited_existing->kind != TypeKind::Unknown &&
                        !is_subtype(info.type, *inherited_existing, &classes_)) {
                        report_incompatible_assignment(node, info.type, *inherited_existing,
                                                       "variable");
                    }
                    classes_.declare_member(current_class_qualified_name_,
                                            target_name->identifier(), info.type,
                                            node.span().start_line);
                } else if (!already_method) {
                    classes_.declare_member(current_class_qualified_name_,
                                            target_name->identifier(), info.type,
                                            node.span().start_line);
                }
            }
            // A same-name METHOD collision is left unreported here -- a
            // combined method+attribute namespace is a pre-existing gap
            // outside this fix round's scope.
        }
    } else {
        // A non-Name target. The annotation is resolved EXACTLY ONCE across
        // BOTH passes: the eager self-attribute scan
        // (declare_self_attribute_placeholder) resolves and caches a
        // `self.x: T` whose attribute it actually declares, and this reuses
        // that resolution rather than re-invoking AnnotationResolver and
        // re-reporting a bad annotation. Everything else on this path --
        // a `self.x: T` the scan skipped because the name was already
        // declared on this class, and every other non-Name target
        // (`xs[0]: int`, `other.x: int`), which the scan never looks at --
        // is not in the cache and is resolved here for the first and only
        // time.
        const auto cached_self_annotation = self_annotation_types_.find(&node);
        if (cached_self_annotation != self_annotation_types_.end()) {
            info.type = cached_self_annotation->second;
        } else {
            AnnotationResolver resolver(classes_, sink_);
            info.type = resolver.resolve(node.annotation());
        }

        // `self.x: T = ...` inside a method DECLARES the instance
        // attribute, exactly like the plain `self.x = ...` form
        // assign_attribute already handled. Without this branch the
        // annotated form -- close to universal in typed Python -- declared
        // NOTHING, so any later read of self.x was a false attr-defined
        // TypeError on mypy-clean code. The guard and the three-way line
        // disambiguation are assign_attribute's own, shared verbatim rather
        // than copied (see self_attribute_receiver_type / self_member_state);
        // This branch is one of two halves: pre_collect_class_body's
        // own AnnAssign arm is the other, and is what makes a reader
        // method sitting ABOVE the declaring one work too.
        //
        // Every other non-Name target (`xs[0]: int = 5`, `other.x: int = 5`)
        // is unchanged: annotation resolved for `expected` only, no binding,
        // and the shared value check below still runs.
        const auto* target_attribute = dynamic_cast<const ast::Attribute*>(&node.target());
        const std::optional<Type> self_type =
            target_attribute != nullptr ? self_attribute_receiver_type(*target_attribute)
                                        : std::nullopt;
        if (self_type.has_value()) {
            const int line = node.span().start_line;
            const SelfMemberState state = self_member_state(target_attribute->attribute(), line);
            // ExistingDeclaration ALSO covers a same-name METHOD, for which
            // member_type is empty -- left unreported and undeclared here,
            // matching the class-body AnnAssign branch above: a combined
            // method+attribute namespace is a pre-existing gap.
            const std::optional<Type> existing =
                state == SelfMemberState::ExistingDeclaration
                    ? classes_.member_type(current_class_qualified_name_,
                                           target_attribute->attribute())
                    : std::nullopt;
            const bool method_name_collision =
                state == SelfMemberState::ExistingDeclaration && !existing.has_value();
            // WHICH CLASS the earlier declaration lives on decides everything
            // below, because mypy has TWO different rules here, not one --
            // measured against mypy 1.18.1, and an earlier single rule
            // (whichever way its comparison was written) was a false
            // TypeError on one of the two shapes:
            //
            //   INHERITED (`self.v: object` in Base, `self.v: int` in Child):
            //     accepted, and the annotation really does become Child's own
            //     declared type. `reveal_type(self.v)` is "builtins.int" in
            //     EVERY Child method (not just the annotating one) and
            //     "builtins.object" in Base; `self.v = "s"` in Child is an
            //     error while the same line in Base is clean. The narrower
            //     annotation must therefore be INSTALLED on the current class
            //     -- keeping the base's type instead makes Child's own
            //     `self.v + 1` (mypy-clean) a false TypeError.
            //     Widening IS reported, as `expression has type "object",
            //     base class "Base" defined the type as "int"` -- so the
            //     annotation is the `expression` and the inherited
            //     declaration is the `variable`, which is the direction the
            //     comparison and the report below are written in.
            //
            //   SAME CLASS (class-body `n: object` plus `self.n: int = 7` in
            //     a method, or two method-level declarations): the annotation
            //     is IGNORED -- it declares nothing and is not checked for
            //     compatibility at all. `reveal_type(self.n)` stays
            //     "builtins.object" in every other method, a later
            //     `self.n = "s"` is CLEAN, and even the flatly contradictory
            //     `self.n: int = "s"` under a class-body `n: object` is clean
            //     (mypy checks the VALUE against the class-body type, "s"
            //     against object, and never against the annotation).
            //     Installing `int` here would make that clean `self.n = "s"`
            //     a false TypeError -- the exact trade the inherited case
            //     demands in the other direction.
            //
            // (mypy does additionally report `Attribute "n" already defined
            // on line N` for two METHOD-level declarations in one class, and
            // for a class-body plain assignment plus a method declaration,
            // whatever the types. We do not: a missed error is safe, and
            // reporting one would need the class-body/method distinction this
            // branch does not carry. It is NOT reported for the class-body
            // ANNOTATION plus method-annotation pair, which mypy accepts
            // outright.)
            const std::optional<Type> own_existing =
                existing.has_value() ? classes_.own_member_type(current_class_qualified_name_,
                                                                target_attribute->attribute())
                                     : std::nullopt;
            // An Unknown own declaration is not a real one to keep. Nothing
            // can be checked against Unknown and nothing is lost by
            // installing over it, so it falls through to the install path
            // with BrandNew and OwnPlaceholder.
            //
            // The reachable shape is an earlier `self.x = <value this model
            // cannot type>` in a method ABOVE this one -- e.g. a call to a
            // function with no return annotation, verified against the built
            // binary: the attribute reads as the ANNOTATED type in every
            // later method, which is only true because of this
            // fall-through. (An earlier version of this comment also cited "a
            // class-body plain Assign's placeholder Phase 3 has not reached
            // yet, the assignment sitting BELOW this method". That shape is
            // UNREACHABLE, traced and probed: pre_collect_class_body's second
            // loop walks FunctionDefs and class-body Assigns in ONE
            // source-order pass and each declaration is guarded on the member
            // not existing yet, so for the ASSIGN to own the placeholder it
            // must sit ABOVE the method -- and then Phase 3, which is also
            // source-ordered, has already filled in its real type by the time
            // this statement is walked. A placeholder belonging to a method
            // below is at that method's own line, not the assignment's, so it
            // is OwnPlaceholder for its own statement and never
            // ExistingDeclaration here.)
            const bool keep_earlier_declaration =
                own_existing.has_value() && own_existing->kind != TypeKind::Unknown;
            if (keep_earlier_declaration) {
                // The annotation is discarded HERE, not merely left
                // undeclared: `info.type` feeds the value check at the end of
                // this function and the TypeMap entries just below, and both
                // must see the type the attribute actually HAS. That is what
                // makes `self.n: int = "s"` under a class-body `n: object`
                // come out clean (value against object) and `self.n: str =
                // "s"` under a class-body `n: int` report `expression has
                // type "str", variable has type "int"` -- mypy's own message
                // for that program, verbatim.
                info.type = *own_existing;
            } else if (existing.has_value()) {
                // Inherited from a base: a narrowing override, reported only
                // when it is not in fact a narrowing. Unknown on either side
                // means we cannot tell, so nothing is reported and the
                // annotation is installed regardless (below) -- an Unknown
                // annotation replacing a known inherited type silences later
                // reads of it, a missed error rather than a false one.
                if (info.type.kind != TypeKind::Unknown && existing->kind != TypeKind::Unknown &&
                    !is_subtype(info.type, *existing, &classes_)) {
                    report_incompatible_assignment(node, info.type, *existing, "variable");
                }
            } else if (state != SelfMemberState::ExistingDeclaration) {
                // BrandNew or OwnPlaceholder: self_member_state alone can no
                // longer tell "genuinely brand new" from "narrowing a base's
                // declaration" apart, now that the eager member-collection
                // phase installs THIS class's own placeholder for the FIRST
                // self.x: T of a name regardless of whether a base already
                // has it (see declare_self_attribute_placeholder's own
                // comment) -- so `existing` above, which only ever looks at
                // state == ExistingDeclaration, misses the INHERITED case
                // entirely once that placeholder exists. Check the base
                // chain directly, bypassing this class's own (absent, or
                // just-installed) entry, for the identical widening rule the
                // INHERITED case above already states -- excluded when state
                // IS ExistingDeclaration with no `existing` value, since that
                // combination is method_name_collision, not a miss to fall
                // back from.
                const std::optional<Type> inherited_existing = classes_.inherited_member_type(
                    current_class_qualified_name_, target_attribute->attribute());
                if (inherited_existing.has_value() && info.type.kind != TypeKind::Unknown &&
                    inherited_existing->kind != TypeKind::Unknown &&
                    !is_subtype(info.type, *inherited_existing, &classes_)) {
                    report_incompatible_assignment(node, info.type, *inherited_existing, "variable");
                }
            }
            if (!method_name_collision && !keep_earlier_declaration) {
                // Brand new, this statement's own placeholder, or an
                // inherited declaration this annotation overrides on the
                // current class -- in all three the ANNOTATION becomes the
                // declared type. Runs regardless of node.has_value():
                // `self.ys: list[int]` with no value is legal in a method
                // body and still declares the member (verified mypy-clean,
                // and a later read of it too).
                classes_.declare_member(current_class_qualified_name_,
                                        target_attribute->attribute(), info.type, line);
            }
            // type_of_attribute never ran for this target, so the two TypeMap
            // entries it would have written are recorded by hand, exactly as
            // assign_attribute does -- a missing entry is a visible hole in
            // `--types` output and in the TypedPrinter tests.
            types_.insert(target_attribute, info.type);
            types_.insert(&target_attribute->value(), *self_type);
        }
    }

    if (!node.has_value()) {
        // A value-less annotation re-declares the path with nothing to
        // narrow it to -- killed and left at its declared type.
        redeclare_narrowing(node.target(), std::nullopt);
        return;
    }
    if (info.redefinition) {
        // Already reported once; mypy reports ONLY that, never an assignment
        // error alongside it. Still type the value for the TypeMap, but
        // never against the colliding annotation's type.
        typer_.type_of(node.value(), Type::unknown());
        redeclare_narrowing(node.target(), std::nullopt);
        return;
    }
    const Type value_type = typer_.type_of(node.value(), info.type);
    if (value_type.kind != TypeKind::Unknown && info.type.kind != TypeKind::Unknown &&
        !is_subtype(value_type, info.type, &classes_)) {
        report_incompatible_assignment(node, value_type, info.type, "variable");
        // Incompatible with the fresh annotation, so nothing narrows to a
        // type that annotation forbids -- killed and left at the declared
        // (annotated) type, same as the value-less and redefinition arms.
        redeclare_narrowing(node.target(), std::nullopt);
        return;
    }
    // The value checked out against the fresh annotation. Whether that
    // narrows the path is NOT one rule -- verified against mypy 1.18.1, and
    // the two are asymmetric:
    //
    //   A NAME target does NOT narrow from its own annotated-and-valued
    //   declaring statement: `x: object = 5` (function- or module-local, and
    //   a class-body bare name alike) reveals `builtins.object` on the very
    //   next line, and `print(x + 1)` right there is a genuine "Unsupported
    //   operand types" error -- narrowing for a plain variable only ever
    //   comes from a SEPARATE, later plain reassignment (assign_name's own
    //   rule), never from the declaring annotation itself. A bare Name's own
    //   AnnAssign is always this path's first-ever declaration in this scope
    //   (bind_resolved_annotation's non-redefinition outcome means exactly
    //   that, whether freshly bound or filling this same statement's own
    //   placeholder), so there is no reachable case where narrowing it here
    //   would even matter for a genuine re-declaration -- mypy rejects a
    //   second one as `already defined` before any narrowing question arises.
    //
    //   An ATTRIBUTE target DOES narrow from its own declaring statement,
    //   including a BRAND NEW one: `self.n: object = 5` (n not previously
    //   declared at all) reveals `builtins.int` on the next line, and is
    //   Success -- matching the plain `self.n = 5` form assign_attribute
    //   already narrows, and matching the "annotation ignored" / "inherited
    //   override" cases just above, which narrow to the VALUE's type
    //   regardless of which declared type ends up installed.
    if (dynamic_cast<const ast::Name*>(&node.target()) != nullptr) {
        redeclare_narrowing(node.target(), std::nullopt);
    } else {
        redeclare_narrowing(node.target(), value_type);
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
        // real (possibly bogus) type, and mypy still reports it. This was
        // skipped entirely at one point, so
        // `def m() -> Bogus:` inside a class silently swallowed the bad
        // annotation. Resolved for its diagnostic side effect only -- the
        // result feeds nothing, since the function's OWN diagnostic above is
        // already the only thing reported for its (missing) signature.
        if (node.has_return_annotation()) {
            AnnotationResolver resolver(classes_, sink_);
            resolver.resolve(node.return_annotation());
        }
        report(node, DiagnosticKind::TypeCheckerTypeError,
               "method must have at least one argument");
        FunctionScopeGuard guard(scopes_);
        // A real `def` boundary resets the narrowing map -- see
        // NarrowingMap::clear for the full per-construct rule.
        NarrowingScopeGuard narrowing_guard(narrowings_);
        // A `class` statement is legal in even this broken method's body, and
        // its body IS still walked below, so the alias frame belongs here
        // too -- otherwise visit(ClassDef) would find no frame to register in
        // and silently fall back to leaving the local class unreachable by
        // its own bare name.
        LocalClassAliasGuard alias_guard(classes_, local_class_alias_frames_);
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
        FlagGuard function_guard(in_function_body_, true);
        FlagGuard loop_guard(in_loop_body_, false);
        check_suite(node.body());
        return;
    }

    // Every parameter's type, and whether it counts toward "missing an
    // annotation" -- self (a method's own first parameter) is exempt, per
    // mypy's disallow-untyped-defs. A top-level FunctionDef was already
    // resolved once by collect_signatures's Phase 2 (top_level_signatures_),
    // and a METHOD by pre_collect_class_body's own pre-pass
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
        // `args.end() - 1`/`args.back()` are
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
                // A METHOD can now reach this cached branch too
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
                                Type::callable(param_types, return_type,
                                               defaulted_param_count(params)));
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
        report(node, DiagnosticKind::TypeCheckerTypeError,
               "function is missing a type annotation");
    }

    // A wrong-typed default is reported at the `def` line (mypy: code
    // `assignment`, not `arg-type`), each mismatch its own diagnostic like
    // every other N-bad-items rule in this checker. Defaults are typed in
    // the ENCLOSING scope, matching Python's own evaluate-at-def-time
    // semantics -- a default cannot see another parameter of the same def,
    // so this runs entirely BEFORE the Function scope below is pushed.
    //
    // A default is typed WITHOUT the enclosing scope's narrowings, and that
    // is measured rather than inherited from the scope decision above.
    // Verified against mypy 1.18.1: with `x: object` narrowed to `int` on
    // the line before, `reveal_type(x)` is `builtins.int` while
    // `def inner(a: int = x)` on the very next line is
    // `Incompatible default for argument "a" (default has type "object",
    // argument has type "int")` -- the DECLARED type is what mypy checks a
    // default against, even though it narrows the name immediately above.
    // The same holds at module scope, and with `x = "s"` against
    // `a: str = x`, so it is the position and not a quirk of `int`.
    // (CPython runs all three, printing the narrowed value, so this is mypy
    // alone rejecting -- which the union rule still covers.) A short-lived
    // guard rather than moving the loop, because the loop must STAY in the
    // enclosing scope for name resolution; only the narrowings reset.
    // The braces are load-bearing: the reset lasts for the defaults and
    // nothing else, so it neither survives into the body walk (which has a
    // boundary guard of its own) nor leaves two guards restoring the same
    // map in an order a reader has to work out.
    {
        NarrowingScopeGuard defaults_narrowing_guard(narrowings_);
        typer_.set_statement_line(def_line);
        for (std::size_t i = 0; i < params.size(); ++i) {
            const ast::Parameter& parameter = params[i];
            if (parameter.default_value == nullptr) {
                continue;
            }
            const Type default_type = typer_.type_of(*parameter.default_value, param_types[i]);
            if (default_type.kind != TypeKind::Unknown &&
                param_types[i].kind != TypeKind::Unknown &&
                !is_subtype(default_type, param_types[i], &classes_)) {
                report(node, DiagnosticKind::TypeCheckerTypeError,
                      "incompatible default for argument \"" + parameter.name +
                          "\" (default has type \"" + type_name(default_type) +
                          "\", argument has type \"" + type_name(param_types[i]) + "\")");
            }
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
        const Type signature_type =
            Type::callable(param_types, return_type, defaulted_param_count(params));
        const std::vector<std::string> param_names = param_names_of(params);
        Binding signature{signature_type, def_line, /*annotated=*/true};
        // Assigned rather than passed positionally -- see
        // Binding::param_names' own comment.
        signature.param_names = param_names;
        if (scopes_.bound_in_current_scope(node.name())) {
            const Resolution existing = scopes_.resolve(node.name());
            // Routed through is_unfilled_placeholder for
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
            } else if (conditional_defs_.count(&node) != 0 &&
                       has_identical_signature(*existing.binding, signature_type, param_names)) {
                // mypy's CONDITIONAL-FUNCTION-DEFINITION allowance, which is
                // NOT scope-limited -- this site used to have no
                // conditionality test at all, so seven measured shapes both
                // oracles accept drew a false
                // `name "g" already defined on line N`. Measured 2026-09-11
                // (mypy 1.18.1 / Python 3.14.2), all mypy-CLEAN and all
                // CPython-clean, all previously rejected here:
                //   if c: def g ... else: def g              (both arms)
                //   flat def g, then def g inside an `if`
                //   two defs of g in the SAME `if` body
                //   two defs of g in a `while` body
                //   def g in `if c`, def g in `if not c`
                //   flat def g, then def g in a `for` body
                //   flat def g, then def g in a NESTED `if`
                // and an eighth where the first binding is not a def at all:
                //   g = h (h a module-level def), then def g inside an `if`
                //
                // THE TWO CONDITIONS ARE BOTH LOAD-BEARING, and dropping
                // either turns a missed error into a shipped one:
                //
                // 1. `conditional_defs_.count(&node)` -- the allowance is
                //    about the LATER definition only. `def g` then a FLAT
                //    `def g` is `Name "g" already defined` under mypy
                //    (measured, both at module and function scope), and a
                //    later VARIABLE definition is an error even when it is
                //    itself conditional (measured: `if c: def g` then
                //    `if not c: g: int = 1` -> `Name "g" already defined on
                //    line 3`). The latter never reaches here: it is
                //    bind_resolved_annotation's report, deliberately left
                //    alone.
                //
                // 2. `has_identical_signature(...)` -- mypy's allowance is
                //    exactly "identical signatures". Measured:
                //    `if c: def g() -> int ... else: def g(a: int) -> str` ->
                //    `All conditional function variants must have identical
                //    signatures  [misc]`, and `g: int = 1` then a conditional
                //    `def g` -> `Incompatible redefinition (redefinition with
                //    type "Callable[[], int]", original type "int")  [misc]`.
                //    Both keep reporting here; the wording differs from
                //    mypy's, a wording divergence rather than a compliance
                //    one, since both oracles reject the program either way.
                //    The predicate compares `defaulted_params`, so
                //    `def g(a: int = 1)` versus `def g(a: int)` -- mypy's
                //    `identical signatures` error, measured -- still reports,
                //    while differing default VALUES at the same count stay
                //    clean, also matching mypy.
                //
                //    This used to be `existing.binding->type ==
                //    signature_type`, which was wrong in BOTH directions at
                //    once: `operator==` is union-order-sensitive, so
                //    `def g(a: int | str)` against `def g(a: str | int)` drew
                //    a false TypeError on code both oracles accept, and it
                //    carries no parameter names, so `def g(a: int)` against
                //    `def g(b: int)` was silently accepted where mypy reports
                //    `All conditional function variants ...`. Never restore
                //    `==` here; see has_identical_signature's own comment for
                //    why `is_equivalent` is not the answer either.
                //
                // The first binding wins and this one is dropped, matching
                // collect_signatures' own conditional-redefinition arm.
            } else {
                report(node, DiagnosticKind::SemanticAnalyzerTypeError,
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
    // order_exempt=true: a
    // parameter is bound before its body runs, so it can NEVER genuinely be
    // used-before-definition inside that same body -- see Binding::
    // order_exempt's own comment for why the ordinary `>=` ordering check
    // would otherwise misfire on a one-line suite, and why
    // is_unfilled_placeholder needs this same flag to avoid mistaking a
    // same-line parameter for its own placeholder-fill case.
    FunctionScopeGuard guard(scopes_);
    // A real `def` boundary resets the narrowing map -- see
    // NarrowingMap::clear for the full per-construct rule.
    NarrowingScopeGuard narrowing_guard(narrowings_);
    // The frame every function-local `class`
    // statement in THIS body registers its bare-name alias into, torn down
    // (restoring any shadowed outer one) when this body's walk is done -- so
    // a local class's bare name resolves inside its defining function and
    // nowhere else. See local_class_alias_frames_' own comment.
    LocalClassAliasGuard alias_guard(classes_, local_class_alias_frames_);
    // Task 20: `return_type` (resolved above, either freshly or from
    // top_level_signatures_'s cache) becomes the declared type Return's own
    // checks read for the DURATION of this body walk -- restored by
    // ReturnContextGuard's destructor to whatever the ENCLOSING function's
    // (if any) was, so a nested def checks its own returns against its own
    // signature.
    ReturnContextGuard return_guard(current_return_type_, return_type);
    for (std::size_t i = 0; i < params.size(); ++i) {
        const ast::Parameter& parameter = params[i];
        // The bool `bind` returns MUST be checked, exactly as it is for
        // collect_signatures's own def/def-collision bind
        // call -- otherwise `def f(x: int, x: str) -> None` silently binds
        // `x` once (keeping only the FIRST parameter's type) instead of
        // reporting the duplicate. Verified against mypy 1.18.1: `Duplicate
        // argument "x" in function definition`.
        //
        // `method_self` is the ONE place the `is_method` this function
        // already computed becomes a durable fact about the BINDING rather
        // than about the scope -- which is what lets `self.x = ...` declare
        // an attribute from inside a closure that captured a method's self,
        // and refuse to from a nested def's own shadowing `self` parameter.
        // See Binding::method_self for both measurements.
        if (!scopes_.bind(parameter.name, Binding{param_types[i], def_line,
                                                  /*annotated=*/parameter.annotation != nullptr,
                                                  /*order_exempt=*/true,
                                                  /*method_self=*/is_method && i == 0})) {
            report(node, DiagnosticKind::SemanticAnalyzerTypeError,
                  "duplicate argument \"" + parameter.name + "\" in function definition");
        }
    }
    // A nested `def` gets NO collect pass (verified: calling a nested
    // function defined LATER in the same body is used-before-def) -- this
    // pre-bind pass only places PLACEHOLDERS (Unknown) so the ordering
    // check can tell "used before definition" apart from "not defined"; see
    // pre_bind_function_body's own comment.
    pre_bind_function_body(node.body());
    // Classify every nested `def` in THIS body as conditional or not, before
    // the body walk reaches any of them -- see conditional_defs_' comment and
    // the redefinition rule at the binding site above.
    for_each_flat_statement(node.body(), /*directly_in_body=*/true,
                            [this](const ast::Stmt& statement, bool at_flat_top_level) {
                                const auto* nested =
                                    dynamic_cast<const ast::FunctionDef*>(&statement);
                                if (nested != nullptr && !at_flat_top_level) {
                                    conditional_defs_.insert(nested);
                                }
                            });
    {
        FlagGuard function_guard(in_function_body_, true);
        FlagGuard loop_guard(in_loop_body_, false);
        check_suite(node.body());
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
        report(node, DiagnosticKind::TypeCheckerTypeError, "missing return statement");
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
        // This top-level ClassDef is a LOSING
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
        // A ClassDef lexically inside a `def` (or any
        // other non-module, non-class scope) was never seen by
        // collect_classes' Phase-1 walk either, for the identical reason.
        //
        // An earlier version declared a NON-colliding
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
        // when nothing else currently uses it.
        //
        // Stopping at that isolation alone, though,
        // made EVERY function-local class construction a FALSE NameError --
        // `def make(): class Local: ...; v = Local()` is mypy-clean
        // (verified against mypy 1.18.1) and reported `name 'Local' is not
        // defined`, because constructor dispatch
        // (ExpressionTyper::type_of_name_call's classes_.is_class(identifier)
        // lookup, expression_typer_calls.cpp) looks up the literal
        // source-level identifier and has no scope awareness of its own.
        // That traded a NARROW false attr-defined for a BROAD false
        // NameError -- strictly worse, and "a missed error beats a false
        // one" does not apply when the outcome is itself a false diagnostic.
        //
        // Isolation of the ClassTable KEY is therefore kept
        // (it is what closes the cross-function overwrite and the
        // module-level leak); what is added is a SCOPE-LIMITED ALIAS from
        // the class's BARE source-level name to that isolated key, installed
        // here and removed by LocalClassAliasGuard when the ENCLOSING
        // function's body walk finishes. Every read query in ClassTable --
        // is_class, member_type, method_type, constructor_type,
        // bases_of, inherits_builtin -- funnels through canonical_name, so
        // one alias makes all of them, plus AnnotationResolver's own
        // Type::class_of(canonical_name(...)) and is_subtype's class-chain
        // walk, agree on the same isolated entry with no per-consumer
        // change. Outside that function the alias is gone, so a
        // module-level `L()` after the `def f` that declares `class L` still
        // reports the NameError mypy reports for it.
        //
        // A class's own name is never bound into ScopeStack (see the
        // class-level comment for why two other checks depend on that),
        // so this class statement is otherwise invisible to every
        // redefinition check in this file -- but a SAME-scope binding this
        // class's name collides with (a parameter, most commonly a method's
        // own `self`/`this`) is very much visible, right here, before
        // isolation ever runs. Measured 2026-09-13: `class self: pass`
        // written directly in a method's own body, colliding with that
        // method's own first parameter, is mypy `Name "self" already defined
        // on line N  [no-redef]` regardless of whether anything ever reads an
        // attribute through the shadowed name afterward (unlike the
        // self-attribute placeholder half of this same defect, fixed
        // separately in pre_collect_class_body -- see
        // receiver_rebound_in_own_scope's own comment -- this check does not
        // depend on there being a reader at all, or on the store coming
        // before or after this statement: mypy's redefinition rule is
        // POSITION-INDEPENDENT, and bound_in_current_scope already reflects
        // the whole scope by the time any statement in it is reached). Scoped
        // deliberately narrow: this only ever fires against a binding
        // ScopeStack actually holds (a parameter, a plain assignment, a
        // nested def), never against ANOTHER class of the same name, since
        // no class's name is ever bound here for a second one to collide
        // with -- that broader gap (two same-named classes, or two same-named
        // methods, colliding with each other) is a separate, still-open
        // defect; see CLAUDE.md.
        if (scopes_.bound_in_current_scope(node.name())) {
            const Resolution existing = scopes_.resolve(node.name());
            report(node, DiagnosticKind::SemanticAnalyzerTypeError,
                  "name \"" + node.name() + "\" already defined on line " +
                      std::to_string(existing.binding->declared_line));
        }
        qualified_name = declare_isolated_class(
            node, "<local-class>#" + std::to_string(node.span().start_line) + "#" + node.name());
        if (!local_class_alias_frames_.empty()) {
            // Installed AFTER declare_isolated_class, so the entry the alias
            // points at already exists. The frame records the bare name with
            // whatever it previously resolved to, so teardown RESTORES a
            // shadowed outer local class rather than deleting it -- see
            // LocalClassAliasGuard. The emptiness guard is not decoration: a
            // frame is pushed by visit(FunctionDef) alone, so a ClassDef
            // reaching this branch from some other non-module, non-class
            // scope would have nowhere to register a removal and must not
            // install an alias that then leaks forever.
            local_class_alias_frames_.back().emplace_back(
                node.name(), classes_.declare_scoped_alias(node.name(), qualified_name));
        }
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
    // A nested class body ALSO resets the narrowing map -- see
    // NarrowingMap::clear for the full per-construct rule and the measured
    // reason a class body is a boundary exactly like a `def` is.
    NarrowingScopeGuard narrowing_guard(narrowings_);
    // Declares every method signature and
    // placeholder-declares every attribute BEFORE any of this class's own
    // body statements are walked for real -- see pre_collect_class_body's
    // own comment for the full mechanism. Now a no-op for an ordinary class
    // (the module-wide member-collection phase in visit(Module) already ran
    // it), and still the REAL pass for a function-local or collision-losing
    // class, which that module-wide phase never sees.
    pre_collect_class_body(node, qualified_name);
    FlagGuard function_guard(in_function_body_, false);
    FlagGuard loop_guard(in_loop_body_, false);
    check_suite(node.body());
}

void TypeChecker::pre_collect_class_body(const ast::ClassDef& node,
                                         const std::string& qualified_name) {
    if (!pre_collected_.insert(&node).second) {
        // Already covered by the module-wide member-collection phase (see
        // visit(Module)). Running the body again would re-invoke
        // AnnotationResolver on every annotation in it and report each bad
        // one a second time -- the same double-report hazard the
        // class_body_annotation_types_ and class_method_signatures_ caches
        // exist to prevent for the ordinary walk's own later visit.
        return;
    }

    // Every direct class-body AnnAssign is resolved
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
    for_each_flat_statement(node.body(), /*directly_in_body=*/true,
                            [&](const ast::Stmt& statement, bool) {
        if (const auto* ann_assign = dynamic_cast<const ast::AnnAssign*>(&statement)) {
            if (const auto* target_name = dynamic_cast<const ast::Name*>(&ann_assign->target())) {
                AnnotationResolver resolver(classes_, sink_);
                const Type type = resolver.resolve(ann_assign->annotation());
                class_body_annotation_types_.emplace(ann_assign, type);
                // own_member_type, not the chain-walking member_type: a
                // class-body annotation that NARROWS a base's declaration is
                // itself this class's declaration and must be installed --
                // verified against mypy 1.18.1, `class Base: v: object` /
                // `class Child(Base): v: int` reveals `self.v` as
                // `builtins.int` inside Child, including in a method written
                // ABOVE the annotation. Gating on member_type skipped the
                // install whenever a base already declared the name, so the
                // reader saw the base's type and `self.v + 1` was a false
                // TypeError. A SECOND class-body annotation of the same name
                // in the SAME class is still skipped, which is what this
                // guard was always for.
                if (!classes_.own_member_type(qualified_name, target_name->identifier())
                         .has_value() &&
                    !classes_.method_type(qualified_name, target_name->identifier()).has_value()) {
                    classes_.declare_member(qualified_name, target_name->identifier(), type,
                                            ann_assign->span().start_line);
                }
            }
        }
    });

    for_each_flat_statement(node.body(), /*directly_in_body=*/true,
                            [&](const ast::Stmt& statement, bool) {
        if (const auto* function_def = dynamic_cast<const ast::FunctionDef*>(&statement)) {
            if (function_def->params().empty()) {
                // A method with no parameters at all (missing self) is
                // reported by visit(FunctionDef) itself, which never
                // registers it in ClassTable either -- this pre-pass must
                // not add signature information for a method that will
                // never really have one.
                return;
            }
            const Type signature = resolve_method_signature(*function_def, qualified_name);
            class_method_signatures_.emplace(function_def, signature);
            classes_.declare_method(qualified_name, function_def->name(), signature);
            // The receiver can be rebound directly in THIS method's own body
            // too, not only inside a closure nested within it -- measured
            // 2026-09-13: `class self: pass` (or `self = Bag()`, or `for self
            // in [Bag(), Bag()]:`) anywhere in `m`'s own body, with a reader
            // method `read` placed above it, was COMPLETELY SILENT here even
            // though mypy reports (`no-redef`, or is outright clean for the
            // Assign/For forms while CPython still raises AttributeError) and
            // CPython raises AttributeError at the read -- because the
            // shadow check below was only ever consulted when deciding
            // whether to DESCEND INTO a nested def, never for a rebinding at
            // the SAME level as the self.x = ... this pre-pass is about to
            // scan. receiver_rebound_in_own_scope is position-independent
            // (mypy's own redefinition check is: a store BEFORE the rebind
            // statement is caught exactly like one after), so this check
            // must run before scanning the method's body at all, not only
            // before descending into a nested one.
            if (!receiver_rebound_in_own_scope(function_def->params().front().name,
                                               function_def->body())) {
                collect_self_attribute_placeholders(qualified_name,
                                                    function_def->params().front().name,
                                                    function_def->body());
            }
        } else if (const auto* assign = dynamic_cast<const ast::Assign*>(&statement)) {
            if (const auto* target_name = dynamic_cast<const ast::Name*>(&assign->target())) {
                // A plain class-body Assign
                // placeholder-declares the SAME way self.x = ... does (real
                // type filled in later, by assign_to's own is_new_definition
                // check, when Phase 3 actually reaches this statement) --
                // NOT eagerly typed here, since its value expression may
                // itself reference another class-body name whose real,
                // order-sensitive resolution must stay entirely within
                // Phase 3's single pass.
                //
                // Deliberately still the chain-walking member_type, unlike
                // the annotation sub-pass above: assign_to's class-body arm
                // reaches its inherited-widening check only when no own
                // declaration exists (`class Base: v: int` /
                // `class Child(Base): v = "s"` is a real mypy error), so
                // installing a placeholder here would silence it.
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
    });
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
    return Type::callable(param_types, return_type, defaulted_param_count(params));
}

void TypeChecker::declare_self_attribute_placeholder(const std::string& qualified_name,
                                                     const std::string& receiver_name,
                                                     const ast::Expr& target, int line,
                                                     const ast::AnnAssign* annotated_statement) {
    const auto* attribute = dynamic_cast<const ast::Attribute*>(&target);
    if (attribute == nullptr) {
        return;
    }
    const auto* receiver = dynamic_cast<const ast::Name*>(&attribute->value());
    if (receiver == nullptr || receiver->identifier() != receiver_name) {
        return;
    }
    if (classes_.method_type(qualified_name, attribute->attribute()).has_value()) {
        return;
    }
    // See the header: the annotated form declares over an INHERITED name, the
    // plain form does not.
    const bool annotated = annotated_statement != nullptr;
    const bool already_declared =
        annotated ? classes_.own_member_type(qualified_name, attribute->attribute()).has_value()
                  : classes_.member_type(qualified_name, attribute->attribute()).has_value();
    if (already_declared) {
        // Nothing is resolved and nothing is cached on this path, so
        // visit(AnnAssign) resolves this annotation itself when the walk
        // reaches it -- still exactly once, just in the other pass.
        return;
    }
    if (!annotated) {
        classes_.declare_member(qualified_name, attribute->attribute(), Type::unknown(), line);
        return;
    }
    // Resolved ONCE, here, and cached under this exact statement so
    // visit(AnnAssign) reuses it rather than re-resolving (and re-reporting
    // a bad annotation). Declaring the resolved type rather than Unknown is
    // the whole point: see the header for the incompatible-assignment error
    // an absorbing Unknown swallows. The LINE is still this statement's own,
    // so self_member_state's "is this entry my own statement's" test is
    // unaffected.
    AnnotationResolver resolver(classes_, sink_);
    const Type annotated_type = resolver.resolve(annotated_statement->annotation());
    self_annotation_types_.emplace(annotated_statement, annotated_type);
    classes_.declare_member(qualified_name, attribute->attribute(), annotated_type, line);
}

void TypeChecker::collect_self_attribute_placeholders(const std::string& qualified_name,
                                                       const std::string& receiver_name,
                                                       const std::vector<ast::StmtPtr>& body) {
    for (const ast::StmtPtr& statement : body) {
        if (const auto* assign = dynamic_cast<const ast::Assign*>(statement.get())) {
            declare_self_attribute_placeholder(qualified_name, receiver_name, assign->target(),
                                               assign->span().start_line,
                                               /*annotated_statement=*/nullptr);
        } else if (const auto* ann_assign = dynamic_cast<const ast::AnnAssign*>(statement.get())) {
            // The ANNOTATED form, `self.x: T = ...` (and the
            // value-less `self.x: T`), placeholder-declares through the exact
            // same helper as the plain form above -- not a second copy of it,
            // so the two forms cannot drift into recognising different sets
            // of targets. Without this arm a reader method sitting ABOVE the
            // declaring one was still a false attr-defined TypeError even
            // with visit(AnnAssign)'s own branch in place, because
            // nothing had declared the attribute by the time the reader was
            // walked.
            //
            // The annotation IS resolved here, and the resolution cached, so
            // the placeholder carries the REAL declared type rather than an
            // absorbing Unknown -- an Unknown here silences every earlier
            // `self.x = <wrong type>` in the same class, which is a DROPPED
            // error rather than a missed refinement. Caching under the node
            // is what keeps a bad annotation from being reported twice; the
            // placeholder's LINE is still this statement's own, so
            // self_member_state's disambiguation is untouched. Passing the
            // node (rather than a bool) is what carries both halves.
            declare_self_attribute_placeholder(qualified_name, receiver_name, ann_assign->target(),
                                               ann_assign->span().start_line,
                                               /*annotated_statement=*/ann_assign);
        } else if (const auto* if_stmt = dynamic_cast<const ast::If*>(statement.get())) {
            collect_self_attribute_placeholders(qualified_name, receiver_name, if_stmt->body());
            collect_self_attribute_placeholders(qualified_name, receiver_name, if_stmt->orelse());
        } else if (const auto* while_stmt = dynamic_cast<const ast::While*>(statement.get())) {
            collect_self_attribute_placeholders(qualified_name, receiver_name, while_stmt->body());
            collect_self_attribute_placeholders(qualified_name, receiver_name, while_stmt->orelse());
        } else if (const auto* for_stmt = dynamic_cast<const ast::For*>(statement.get())) {
            collect_self_attribute_placeholders(qualified_name, receiver_name, for_stmt->body());
            collect_self_attribute_placeholders(qualified_name, receiver_name, for_stmt->orelse());
        } else if (const auto* nested_def =
                       dynamic_cast<const ast::FunctionDef*>(statement.get())) {
            // A closure CAPTURES the enclosing method's first parameter, and
            // mypy attributes a store through it to the method's own binding
            // at ANY nesting depth -- measured 2026-09-12: a reader ABOVE a
            // closure that assigns `self.q` is mypy Success and CPython
            // prints 1, where this scan's refusal to descend left the
            // attribute undeclared until Phase 3's walk reached it, i.e.
            // AFTER the reader.
            //
            // Unless the nested def REBINDS the name, in which case the
            // store is not the method's. The shadowing parameter's
            // ANNOTATION is irrelevant: `def inner(self: Bag)` inside a Bag
            // method is still not a declaration, and mypy reports at both
            // the store and the read. So the test is rebinding alone.
            //
            // Rebinding is not just a PARAMETER, though: Python makes a name
            // local to a function scope by ANY assignment to it there, not
            // only by it appearing as a parameter. `def inner(): self =
            // Bag(); self.q = 1` rebinds
            // `self` to a local exactly as a shadowing parameter would, and
            // mypy refuses to attribute that store to the enclosing method's
            // receiver (two attr-defined errors) while CPython raises
            // AttributeError -- checking parameters alone let this scan
            // descend anyway and placeholder-declare "q" as Unknown, which is
            // absorbing and so silently accepted a program BOTH oracles
            // reject. receiver_rebound_in_own_scope covers Assign, AnnAssign,
            // a `for` target, and a nested def/class's OWN name (`def self():
            // ...` / `class self: ...` inside the closure rebinds "self" in
            // the closure's own scope exactly as an assignment would), while
            // still never descending into that nested def/class's OWN body --
            // see the helper's own comment for what conflating those two
            // questions made silent.
            bool shadows = false;
            for (const ast::Parameter& param : nested_def->params()) {
                if (param.name == receiver_name) {
                    shadows = true;
                    break;
                }
            }
            if (!shadows) {
                shadows = receiver_rebound_in_own_scope(receiver_name, nested_def->body());
            }
            if (!shadows) {
                collect_self_attribute_placeholders(qualified_name, receiver_name,
                                                    nested_def->body());
            }
        }
        // A nested ClassDef is still out of reach, and deliberately: its
        // methods' first parameter is the INNER class's, so a store there
        // declares onto that class, not this one. Both oracles reject a
        // program that assumes otherwise.
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

    // NARROWING JOIN. Each branch is walked from the SAME pre-`if` state, so
    // one branch's narrowing never leaks into its sibling, and the state
    // afterwards is the union over both edges. An `if` with no `else` needs
    // no special case: restoring `before` and walking an empty orelse leaves
    // exactly `before`, which is the fall-through edge, and an edge that
    // never assigned a path contributes that path's DECLARED type -- which is
    // what makes `if f: self.n = 7` on an `object`-declared attribute widen
    // back to `object` rather than leaking the branch's `int` past the `if`.
    // Verified against mypy 1.18.1: `builtins.int | builtins.str` for a
    // genuine two-branch split, `builtins.int` when both branches agree, and
    // `builtins.object` for the else-less case.
    const NarrowingState before = narrowings_.snapshot();
    check_suite(node.body());
    const NarrowingState after_body = narrowings_.snapshot();

    narrowings_.restore(before);
    check_suite(node.orelse());
    const NarrowingState after_orelse = narrowings_.snapshot();

    // A BRANCH CONTROL ALWAYS LEAVES CONTRIBUTES NO EDGE. Its end-of-branch
    // state is unreachable at the merge, and taking it anyway is not merely
    // imprecise -- it is a FALSE TypeError, because a branch that never
    // assigned the path contributes that path's DECLARED type and so widens
    // a narrowing the surviving branch established. Measured against mypy
    // 1.18.1 and CPython 3.13.5:
    //
    //   def m(f: bool) -> int:
    //       n: object = object()
    //       if f:
    //           n = 7
    //       else:
    //           return 0
    //       return n + 1
    //
    // mypy: `Success`; CPython: prints `8` then `0`. Taking the returning
    // edge yields `unsupported operand types for + ("object" and "int")` on
    // the last line -- rejecting a program both oracles accept. The same
    // holds with the branches swapped, and for a `self.` attribute path in
    // place of the local.
    //
    // WHEN DROPPING WOULD LEAVE NO EDGES (a branch each way leaves), BOTH ARE
    // KEPT, and the guarantee that makes that safe is NARROW -- state it
    // precisely, because an earlier version of this comment claimed "the
    // merge is unreachable, so whatever state it carries is never read",
    // which was false at the time it was written and is the reason this
    // defect recurred three times. What is actually true:
    //
    //  - The state IS read. check_suite keeps walking the rest of the suite,
    //    because mypy's own semantic analyzer does (see check_suite's
    //    comment for the measurements) -- so every statement after this `if`
    //    is still typed against this state.
    //  - WHEREVER THE TERMINATOR IS LEGAL PYTHON, it cannot produce a
    //    type-CHECKER diagnostic. There, statement_always_leaves agrees with
    //    the decision made here, so check_suite has the suppression live for
    //    the whole remainder of the suite, nested subtrees included.
    //
    //    THE CAVEAT, and it is load-bearing twice over. (i) This call passes
    //    always_leaves_branch its CONTEXT-FREE DEFAULTS (in_function =
    //    in_loop = true) while check_suite passes the REAL context, so at
    //    module or class-body scope the two DISAGREE: the fallback fires,
    //    no region opens, and this join's state is read by a genuinely
    //    type-checked statement. Measured against mypy 1.18.1 and CPython
    //    3.14.2 on `n: object = object()` / `total: int = 0` / `if total > 0:
    //    n = 7; return / else: return` / `total = total + n + 1` at MODULE
    //    scope: cythonpp reports `8:9: TypeError: unsupported operand types
    //    for + ("int" and "object")`. No union-rule violation, because every
    //    such program is one BOTH oracles reject (mypy `"return" outside
    //    function [misc]`, exit 1; CPython `SyntaxError: 'return' outside
    //    function` at compile time) -- but the guarantee is conditional, not
    //    absolute, and the absolute phrasing is what let this defect recur.
    //    (ii) The suppression covers DiagnosticKind::TypeCheckerTypeError
    //    only. A SemanticAnalyzerTypeError, NameError, NotImplementedError or
    //    OverflowError in the remainder still reports, by design -- see
    //    diagnostic_kind.h.
    //  - It does not escape the suite as a narrowing -- subject to the SAME
    //    context caveat: the restore lives in check_suite, so it happens only
    //    where check_suite's own context agrees that the suite went
    //    unreachable. Where it does, check_suite restores the state as of
    //    this `if` before the suite walk returns, so the enclosing
    //    construct's end-of-suite snapshot is this join's result, not
    //    whatever the dead statements after it narrowed.
    //
    // AND THE FALLBACK ITSELF IS THE RIGHT VALUE, not merely a harmless one.
    // It is the union over the edges by which control ACTUALLY leaves, which
    // is the only non-arbitrary state available -- and for the both-arms-
    // `break` case it is exactly the state the enclosing loop's exit sees,
    // since those two break edges ARE that loop's exit edges. The two
    // alternatives are both worse: restoring the pre-`if` state reproduces
    // the very false TypeError above (measured -- delete the whole `if` from
    // the fixture at the top of this comment and the same message lands on
    // the same operand pair, because the pre-`if` state is where `n` is still
    // its declared `object`), and dropping the fallback so `edges` stays
    // empty is that same thing with less information still, since
    // join_narrowings over zero edges returns an empty state, i.e. every path
    // back to its declared type.
    //
    // THREE TERMINATORS, and all three are handled here: `return`, `break`
    // and `continue`. `break` and `continue` are both parsed, so both reach
    // this join, and both produce the identical false TypeError -- measured
    // against mypy 1.18.1 and CPython 3.14.2 on the loop-bodied form of the
    // shape above (`for _ in xs: / if f: n = 7 / else: continue / total =
    // total + n + 1`): mypy `Success`, CPython prints `16` then `0`, and
    // taking the `continue` edge yields `unsupported operand types for +
    // ("int" and "object")`. Identical with `break` in place of `continue`,
    // and identical again with a `while` in place of the `for`. The fourth
    // terminator, `raise`, is the one the parser genuinely does not admit
    // yet -- `if f: ... else: raise ...` is masked purely by that gap, so
    // whoever lands `raise` must add it to statement_always_leaves.
    //
    // The predicate is always_leaves_branch and NOT always_returns, on
    // purpose: see its declaration for why widening always_returns instead
    // would break the missing-return check, which depends on a `break` not
    // counting as a return.
    //
    // NOT EXTENDED TO THE LOOP JOINS BELOW, deliberately. The `while`/`for`
    // equivalent of this shape lands on `NotImplementedError: operations on a
    // union-typed value require narrowing`, which is inside the sanctioned
    // "this compiler cannot model it" code rather than a false TypeError, and
    // is already better than the pre-join behaviour.
    //
    // `classes_` must be threaded through: without it, a joined `Sub | Base`
    // against a declared `Base` is not recognised as equivalent to it, so it
    // is kept as a stored Union -- and a Union operand defers every operator
    // applied to it, turning otherwise-clean code into a false
    // NotImplementedError.
    std::vector<NarrowingState> edges;
    if (!always_leaves_branch(node.body())) {
        edges.push_back(after_body);
    }
    if (!always_leaves_branch(node.orelse())) {
        edges.push_back(after_orelse);
    }
    if (edges.empty()) {
        edges = {after_body, after_orelse};
    }
    narrowings_.restore(join_narrowings(
        edges, [this](const NarrowedPath& path) { return declared_type_of_path(path); },
        &classes_));
}

void TypeChecker::visit(const ast::While& node) {
    // Same reasoning as If: truthiness is universal, so the condition is
    // typed and never checked against anything.
    typer_.set_statement_line(node.span().start_line);
    typer_.type_of(node.condition(), Type::unknown());

    // NARROWING JOIN, ONE FORWARD PASS. The body starts from the pre-loop
    // state, which is what lets a narrowing established before the loop reach
    // into it (measured: mypy does too). Afterwards the state is the union of
    // the pre-loop state -- the edge where the body ran zero times -- and the
    // end-of-body state, which is what makes `self.n = 7` before a loop whose
    // body assigns a str come out as `int | str` after it, exactly as mypy
    // reports.
    //
    // ONE MEASURED DIVERGENCE, accepted deliberately: at the LOOP HEAD mypy
    // sees the fixpoint (`int | str`) where this sees the pre-loop narrowing
    // (`int`), so an int-only operation at the head that mypy rejects is
    // accepted here. A MISSED error, the invariant-safe direction. Reaching
    // the fixpoint needs a second walk of the body, and a walk of this body
    // re-runs every side effect it has -- every diagnostic reported, every
    // name bound, every member declared -- so the second pass would
    // double-report all of them. The other single-pass option, dropping to
    // the declared type on loop entry, would REJECT the mypy-clean
    // `self.n = 7` / `while f: print(self.n + 1); self.n = 8`; a false
    // TypeError is never an acceptable trade for a missed one.
    const NarrowingState before = narrowings_.snapshot();
    {
        FlagGuard loop_guard(in_loop_body_, true);
        check_suite(node.body());
    }
    const NarrowingState after_body = narrowings_.snapshot();
    narrowings_.restore(join_narrowings(
        {before, after_body},
        [this](const NarrowedPath& path) { return declared_type_of_path(path); }, &classes_));

    // The `else` suite runs when the loop exits normally, so it sees the
    // joined state.
    //
    // A MID-BODY `break` OR `continue` EDGE INTO THIS JOIN IS NOT MODELLED,
    // and that is a MEASURED GAP, not merely an unexplored one. Against mypy
    // 1.18.1 and CPython 3.14.2, on `self.n = 7` / `while f: self.n = "s";
    // if f: break; self.n = 8` / `return self.n + 1` called with `f=False`:
    // mypy reports `Unsupported operand types for + ("str" and "int")` with
    // a left operand of `str | int`, CPython runs the file (printing `8`),
    // and this checker is SILENT -- because only the end-of-body state
    // reaches the join, and the body ends on `self.n = 8`, agreeing with the
    // pre-loop `int`. The same shape with `continue` in place of `break`
    // measures the same three ways, except that mypy spells the very same
    // left operand `int | str` there rather than `str | int` -- its union
    // member ORDER tracks the order the edges merge in and is not
    // canonicalised, so quoting it is only ever safe per exact shape. Both
    // are missed errors: the safe direction, but still gaps.
    // (Note this gap is about the edge a mid-body jump carries INTO this
    // loop join. It is unrelated to visit(If)'s own join, which does treat
    // `break` and `continue` as branch terminators.)
    // `test_files/semantic/error_narrowing_misses_a_break_edge.py` pins the
    // silence so closing it is visible as a corpus change rather than as a
    // surprise. Also measured, and NOT gaps: the same shape written with a
    // `while/else` or a `for/else` suite instead of a mid-body jump draws a
    // diagnostic on mypy's line (a NotImplementedError for the joined
    // `int | str` operand, which is the sanctioned "cannot model" code).
    check_suite(node.orelse());
}

void TypeChecker::visit(const ast::For& node) {
    const int line = node.span().start_line;
    typer_.set_statement_line(line);
    const Type iterable_type = typer_.type_of(node.iterable(), Type::unknown());
    // Routed through ExpressionTyper::element_type_of -- the SAME apply()
    // switch type_of_list_comp uses for its own, identical need -- rather
    // than a second copy of the three-way RuleResult handling here.
    const Type element = typer_.element_type_of(node.iterable(), iterable_type);

    // NARROWING JOIN, ONE FORWARD PASS -- same shape as While, see its own
    // comment for the measured loop-head divergence this accepts
    // deliberately. The snapshot is taken BEFORE the target is bound below,
    // because the zero-iteration edge is exactly the state in which the
    // target was never assigned: on that edge the target keeps whatever it
    // held before the loop. Measured against mypy 1.18.1 and CPython 3.13.5:
    //
    //   def m(xs: list[int]) -> int:
    //       x: object = "s"
    //       for x in xs:
    //           print(x)
    //       return x + 1
    //
    // mypy: `Unsupported operand types for + ("object" and "int")` on the
    // last line; CPython: prints `1`, `2`, `3` (so the union rule says this
    // program must draw a diagnostic). Snapshotting after the bind puts the
    // loop's element type on BOTH edges, the join collapses to `int`, and
    // the read comes out clean -- silently accepting what mypy rejects.
    // Snapshotting before puts the pre-loop `object` on the zero-iteration
    // edge, so `object | int` joins back to the declared `object` and the
    // error reports at mypy's line. The case that argues for the other
    // ordering -- a target with NO prior binding, `for x in xs: print(x)`
    // then `x + 1`, which mypy accepts -- was measured too and stays clean
    // under this ordering: with nothing bound before the loop there is no
    // pre-loop entry for the join to widen against.
    const NarrowingState before = narrowings_.snapshot();

    if (const auto* tuple_target = dynamic_cast<const ast::TupleExpr*>(&node.target())) {
        // mypy ACCEPTS a tuple target (`for a, b in pairs:` is mypy-clean),
        // so this must be NotImplementedError, not TypeError -- element_type
        // of a tuple[K, V] is the UNION K | V, not a positional pair, so
        // there is nothing correct to bind a/b to element-wise. Mirrors
        // type_of_list_comp's identical tuple-target arm.
        report(*tuple_target, DiagnosticKind::NotImplementedError,
              "tuple targets in for loops are not supported");
        // Bind each element to Unknown anyway, the
        // same precedent assign_tuple's own arity-mismatch fallback sets --
        // otherwise the body is still walked (unlike a `pass`-only fixture, a
        // real body reading `a`/`b` here would see them wholly UNBOUND) and a
        // real use cascades its own NameError on top of this
        // NotImplementedError instead of being silently absorbed like every
        // other "unsupported shape" case in this checker. order_exempt=true
        // for the same reason the Name-target arm below needs it: bound
        // before the body runs, so a one-line suite reading one right back
        // can never be a genuine use-before-definition.
        //
        // A tuple for-target's own elements are pre-bound too, since
        // 2026-09-13 (pre_bind_function_body routes through the same
        // for_each_bound_name a plain Name target uses), so the ordinary
        // fresh-bind branch below no longer fires for one inside a function
        // body -- this must fill THAT placeholder in directly, exactly as
        // assign_name's own is_unfilled_placeholder branch does for a plain
        // Name target, or the element's order_exempt=false placeholder would
        // survive untouched and misfire the same false "used before
        // definition" order_exempt exists to prevent.
        for (const ast::ExprPtr& element : tuple_target->elements()) {
            if (const auto* name = dynamic_cast<const ast::Name*>(element.get())) {
                if (scopes_.bound_in_current_scope(name->identifier())) {
                    const Resolution existing = scopes_.resolve(name->identifier());
                    if (is_unfilled_placeholder(*existing.binding, line)) {
                        scopes_.rebind(name->identifier(),
                                       Binding{Type::unknown(), line, /*annotated=*/false,
                                              /*order_exempt=*/true});
                    }
                } else {
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
        // order_exempt=true: a for target
        // is bound before its OWN body ever runs, exactly like a parameter --
        // so a one-line suite (`for i in range(3): print(i)`) reading it
        // within that same body is never a genuine use-before-definition.
        // Without this, the body's ExprStmt sets statement_line_ to this same
        // line, and the ordinary `declared_line >= statement_line_` ordering
        // check (declared_line == line here too) misfires exactly like it did
        // for a one-line def's own parameter before that case was fixed.
        // A read BEFORE the loop is untouched by this: the name is not bound
        // at all yet, so it still correctly reports "is not defined".
        assign_name(*name_target, element, line, /*order_exempt=*/true);
    }
    // Any other target shape (Attribute, Subscript) is outside this task's
    // tested scope; nothing to bind.

    {
        FlagGuard loop_guard(in_loop_body_, true);
        check_suite(node.body());
    }
    const NarrowingState after_body = narrowings_.snapshot();
    narrowings_.restore(join_narrowings(
        {before, after_body},
        [this](const NarrowedPath& path) { return declared_type_of_path(path); }, &classes_));

    check_suite(node.orelse());
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
            report(node, DiagnosticKind::TypeCheckerTypeError, "return value expected");
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
        // A value is PRESENT, but that alone is not the error -- returning a
        // None-VALUED expression from a `-> None` function is ordinary,
        // mypy-clean Python:
        //
        //   def maybe(x: int) -> None:
        //       if x < 0:
        //           return None      # mypy-clean
        //   def forward(x: int) -> None:
        //       return g()           # mypy-clean, where g() -> None
        //
        // Verified against mypy 1.18.1: both are accepted, and only a
        // non-None value ("return 5") draws mypy's "No return value
        // expected". So the value's TYPE decides, exactly as it does for
        // every other declared return type below. Unknown is absorbing here
        // as everywhere: the root cause already reported.
        if (value_type.kind != TypeKind::Unknown &&
            !is_subtype(value_type, Type::none(), &classes_)) {
            report(node, DiagnosticKind::TypeCheckerTypeError, "no return value expected");
        }
        return;
    }
    if (value_type.kind != TypeKind::Unknown &&
        !is_subtype(value_type, current_return_type_, &classes_)) {
        // New wording, not in the corpus's pre-existing settled set --
        // verified against mypy 1.18.1's own "Incompatible return value type
        // (got \"str\", expected \"int\")", lower-cased to match this
        // codebase's existing message-casing convention (every other message
        // here starts lower-case despite mypy's own title case).
        report(node, DiagnosticKind::TypeCheckerTypeError,
              "incompatible return value type (got \"" + type_name(value_type) +
                  "\", expected \"" + type_name(current_return_type_) + "\")");
    }
}

bool TypeChecker::contains_reachable_break(const std::vector<ast::StmtPtr>& body,
                                           bool in_function) {
    for (const ast::StmtPtr& statement : body) {
        if (dynamic_cast<const ast::Break*>(statement.get()) != nullptr) {
            return true;
        }
        if (const auto* if_stmt = dynamic_cast<const ast::If*>(statement.get())) {
            // An `if` is not a loop, so a break inside one still belongs to
            // THIS enclosing loop -- look through it.
            if (contains_reachable_break(if_stmt->body(), in_function) ||
                contains_reachable_break(if_stmt->orelse(), in_function)) {
                return true;
            }
        } else if (const auto* for_stmt = dynamic_cast<const ast::For*>(statement.get())) {
            // A nested For/While's own BODY is
            // deliberately not recursed into -- a break there can only ever
            // escape THAT loop, never this one. But its ORELSE is the
            // opposite case: a loop's `else` clause runs OUTSIDE the loop's
            // own break scope (that is precisely why `for x in []: pass` /
            // `else: break` is a top-level `SyntaxError: 'break' outside
            // loop` -- the `else` is not inside the loop it is attached to),
            // so a `break` written there targets the ENCLOSING loop and must
            // count here. The previous version of this comment claimed a
            // break in ANY nested loop construct "can only ever escape that
            // inner loop" -- true of the body, false of the orelse, and this
            // is the fix for that false claim.
            if (contains_reachable_break(for_stmt->orelse(), in_function)) {
                return true;
            }
        } else if (const auto* while_stmt = dynamic_cast<const ast::While*>(statement.get())) {
            if (contains_reachable_break(while_stmt->orelse(), in_function)) {
                return true;
            }
        }
        // Anything that always leaves -- a Return, a Continue, or a
        // compound If/For/While whose OWN break-search above just failed
        // and which itself always leaves (e.g. `if c: return 1 else: return
        // 2`, or a nested `while True: pass` with no break, or a nested
        // `for ... else: return 1`) -- makes everything AFTER it in this
        // suite dead code, which mypy prunes before ever asking about a
        // break: `for x in xs: return 1 / break` with a returning `else` is
        // mypy-clean (measured 2026-09-12), where a plain textual scan would
        // still find that `break` and wrongly call the `else` skippable.
        // Four compound shapes -- an if/else where BOTH arms return, a
        // nested `while True: pass`, a nested `for...else: return`, and an
        // `if c: return 1 else: continue` -- all measured mypy-`Success` and
        // running correctly under CPython, and ALL FOUR previously still
        // drew a false "missing return statement" here, because the old
        // version of this check ran only after the If/For/While arms had
        // each already unconditionally `continue`d past it: the stop was
        // dead code for exactly the compound shapes that matter most
        // (measured 2026-09-12, second round). Restructured as an
        // if/else-if chain (rather than each arm ending in its own
        // `continue`) so every statement kind -- compound or not -- falls
        // through to this ONE check after its own break-search has already
        // had its chance.
        //
        // Checked ONLY here, after the break-search above: a bare `break` is
        // itself a statement that "always leaves" (in_loop is true), and a
        // conditional `if c: break` with no other arm, or `if c: break else:
        // return 1` with a leaving other arm, must still report the break as
        // reachable (measured against mypy 1.18.1 -- both keep "missing
        // return statement"). Testing statement_always_leaves on every
        // statement BEFORE the Break/If/For/While arms, as it may look like
        // it should be ordered, would return early on the break itself, or
        // skip descending into an If whose arms both leave, and silently
        // stop finding real, reachable breaks.
        //
        // `in_function` is a REAL parameter (see the declaration), NOT
        // hardcoded `true`: this predicate is reached from
        // statement_always_leaves's own While/For arms, which check_suite
        // calls at MODULE and CLASS scope too, where a `return` is not
        // legal Python at all. Hardcoding `true` here once made a `return`
        // sitting at module scope (itself a blocking error under both
        // oracles) silently "always leave", suppressing a real diagnostic
        // after it -- measured 2026-09-12, second round: `while True: return
        // / break` then `x: int = "s"` at module scope used to report
        // nothing at all; mypy says `"return" outside function [misc]` and
        // CPython raises `SyntaxError: 'return' outside function`, so both
        // oracles reject it and this checker must not silently swallow the
        // (admittedly different) diagnostic it used to give for the bad
        // assignment. `in_loop` stays hardcoded `true`, unlike `in_function`:
        // this scan only ever runs over a loop's own body, so that flag is
        // correct by construction at every call site, and there is nothing
        // to thread.
        //
        // FunctionDef/ClassDef bodies are new scopes a `break` cannot reach
        // out of at all (and could not legally appear there either), so
        // they are skipped here too -- statement_always_leaves already
        // answers false for both.
        if (statement_always_leaves(*statement, in_function, /*in_loop=*/true)) {
            return false;
        }
    }
    return false;
}

// A loop whose body contains no reachable `break` ALWAYS runs its `else`
// clause -- that is what the `else` means -- so an `else` that always returns
// makes the whole loop always return. Measured against mypy 1.18.1 on
// 2026-09-12: `for x in xs: print(x)` / `else: return 3` as the whole body of
// a `-> int` function is `Success` under mypy and prints `1` then `3` under
// CPython, while this checker used to report `missing return statement`.
//
// Reads ORELSE only, never the body: a `for` body may run zero times, so
// `for x in xs: return 1` with no `else` IS a genuine missing return
// (mypy agrees, and there is a test pinning it).
//
// contains_reachable_break's own descent rule is exactly right here and is
// deliberately reused rather than reimplemented: a break in a NESTED loop's
// body can never escape this loop and must not count (mypy: clean), while one
// in that nested loop's ORELSE targets this loop and must (mypy: error).
// Passes contains_reachable_break's `in_function` as a hardcoded `true`, not
// threaded through as a parameter of its own: this helper is called only
// from always_returns, which is itself only ever invoked on a FunctionDef's
// own body (see always_returns' own comment) -- so `true` is the ONE correct
// value here, unlike at contains_reachable_break's OTHER call sites inside
// statement_always_leaves, which is reached from module/class scope too.
bool TypeChecker::loop_else_always_returns(const std::vector<ast::StmtPtr>& body,
                                           const std::vector<ast::StmtPtr>& orelse) {
    return !orelse.empty() && !contains_reachable_break(body, /*in_function=*/true) &&
           always_returns(orelse);
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
            // always_returns only ever runs on a FunctionDef's own body (see
            // its own declaration comment), so in_function=true here too.
            if (is_literal_true(while_stmt->condition()) &&
                !contains_reachable_break(while_stmt->body(), /*in_function=*/true)) {
                return true;
            }
            if (loop_else_always_returns(while_stmt->body(), while_stmt->orelse())) {
                return true;
            }
            continue;
        }
        if (const auto* for_stmt = dynamic_cast<const ast::For*>(statement.get())) {
            if (loop_else_always_returns(for_stmt->body(), for_stmt->orelse())) {
                return true;
            }
            continue;
        }
        // A For with a returning else (or a While with any non-`True`
        // condition and a returning else) is now handled above via
        // loop_else_always_returns. A For/While with NO else, or one whose
        // else does not return, is assumed skippable (false) -- a
        // deliberately conservative, purely syntactic approximation: a loop
        // that might not run at all (an ordinary `while c:`/`for x in xs:`
        // with no else, or an else that itself falls through) genuinely
        // might not always leave, so answering false is the honest reading
        // without modelling the loop's iteration count. This can only ever
        // answer false where mypy answers true (a missed error), never the
        // reverse.
    }
    return false;
}

bool TypeChecker::always_leaves_branch(const std::vector<ast::StmtPtr>& body, bool in_function,
                                       bool in_loop) {
    // An any-of fold over the per-statement rule below. A hit ANYWHERE in the
    // list counts, not just at the end: whatever follows a `break` in the
    // same suite is dead code, so the branch still never falls through.
    for (const ast::StmtPtr& statement : body) {
        if (statement_always_leaves(*statement, in_function, in_loop)) {
            return true;
        }
    }
    return false;
}

bool TypeChecker::statement_always_leaves(const ast::Stmt& statement, bool in_function,
                                          bool in_loop) {
    // All three terminators the parser admits count, each only where Python
    // permits it to appear at all -- see the header for the four measured
    // rows that make the context checks load-bearing rather than pedantic.
    // `raise` is the fourth terminator and the parser rejects it outright
    // ("raise statements are not supported"), so whoever lands it must add
    // it here (legal in both contexts, unlike these three).
    if (dynamic_cast<const ast::Return*>(&statement) != nullptr) {
        return in_function;
    }
    if (dynamic_cast<const ast::Break*>(&statement) != nullptr ||
        dynamic_cast<const ast::Continue*>(&statement) != nullptr) {
        return in_loop;
    }
    if (const auto* if_stmt = dynamic_cast<const ast::If*>(&statement)) {
        // An `if` with no `else` can always fall through, so it never
        // counts -- that is what makes a `break` guarded by a nested `if`
        // CONDITIONAL, and a conditional terminator must still contribute
        // its edge. The `!if_stmt->orelse().empty() &&` conjunct below is
        // DEFENSIVE, not load-bearing, despite an earlier version of this
        // comment implying otherwise: an empty orelse already makes the
        // trailing `always_leaves_branch(if_stmt->orelse(), ...)` fold over
        // zero statements and return false on its own, so the whole
        // expression is false either way -- deleting the emptiness conjunct
        // changes no test's outcome (verified 2026-09-12: 0 of 1393 tests
        // fail with it removed). It stays for readability: stating the
        // no-else case explicitly is easier to read than deriving it from
        // an empty fold. What IS load-bearing is the AND between the two
        // always_leaves_branch calls -- BOTH arms must leave for the `if`
        // itself to count, since one arm leaving and the other falling
        // through is exactly the conditional-terminator case the comment
        // above describes. Verified 2026-09-12: changing that first `&&` to
        // `||` (`!if_stmt->orelse().empty() || always_leaves_branch(body,
        // ...) && always_leaves_branch(orelse, ...)`, which is
        // `!empty() || (leaves(body) && leaves(orelse))` by precedence)
        // fails 3 of 1393 tests.
        return !if_stmt->orelse().empty() &&
               always_leaves_branch(if_stmt->body(), in_function, in_loop) &&
               always_leaves_branch(if_stmt->orelse(), in_function, in_loop);
    }
    if (const auto* while_stmt = dynamic_cast<const ast::While*>(&statement)) {
        if (is_literal_true(while_stmt->condition()) &&
            !contains_reachable_break(while_stmt->body(), in_function)) {
            // Never exits, so it never falls through to the merge either.
            // No terminator is involved, so this arm needs no context check:
            // `while True: pass` is legal wherever a statement is legal.
            return true;
        }
        // Its `else` runs outside its own break scope, so a terminator
        // there targets the ENCLOSING construct -- which is also why the
        // orelse recursion passes THIS suite's context through unchanged
        // rather than in_loop=true; the `else` itself always runs whenever
        // the body cannot break out. contains_reachable_break gets THIS
        // arm's own real `in_function`, not a hardcoded `true`: check_suite
        // calls statement_always_leaves (and so this arm) at module and
        // class scope too, where `in_function` is false and a `return`
        // inside `while_stmt->body()` is not actually "always leaving" --
        // measured 2026-09-12, second round; see contains_reachable_break's
        // own comment for the reproduction.
        return !contains_reachable_break(while_stmt->body(), in_function) &&
               always_leaves_branch(while_stmt->orelse(), in_function, in_loop);
    }
    if (const auto* for_stmt = dynamic_cast<const ast::For*>(&statement)) {
        return !contains_reachable_break(for_stmt->body(), in_function) &&
               always_leaves_branch(for_stmt->orelse(), in_function, in_loop);
    }
    // A nested loop's own BODY is never recursed into: a `break` or
    // `continue` written there belongs to that loop, so it cannot leave
    // this branch. FunctionDef/ClassDef bodies are new scopes no
    // terminator can reach out of.
    return false;
}

void TypeChecker::check_suite(const std::vector<ast::StmtPtr>& body) {
    // Constructed in place the moment the suite goes unreachable, and
    // destroyed on the way out of this function -- so the region is exactly
    // "the rest of THIS suite", and an enclosing suite's own reachable
    // remainder is unaffected. std::optional::emplace is what lets a
    // non-movable guard have a run-time-decided start.
    std::optional<diagnostics::DiagnosticSuppression> unreachable;
    NarrowingState narrowings_at_terminator;

    for (const ast::StmtPtr& statement : body) {
        statement->accept(*this);
        if (!unreachable.has_value() &&
            statement_always_leaves(*statement, in_function_body_, in_loop_body_)) {
            narrowings_at_terminator = narrowings_.snapshot();
            // What this drops is every DiagnosticKind::TypeCheckerTypeError
            // and nothing else -- see diagnostic_kind.h for the measurement
            // that splits cythonpp's own "TypeError" spelling in two, and
            // why the split is an enumerator rather than a flag.
            unreachable.emplace(sink_);
        }
    }

    if (unreachable.has_value()) {
        narrowings_.restore(std::move(narrowings_at_terminator));
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
        // Reuse builtin_call_table.h's exported
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

void TypeChecker::report(const ast::Node& at, DiagnosticKind kind, std::string message) {
    const ast::SourceSpan span = at.span();
    sink_.report_error(diagnostic_code(kind), std::move(message), span.start_line,
                       span.start_column, suppressibility_of(kind));
}

void TypeChecker::report_incompatible_assignment(const ast::Node& at, const Type& value_type,
                                                 const Type& target_type,
                                                 const char* target_label) {
    report(at, DiagnosticKind::TypeCheckerTypeError,
          "incompatible types in assignment (expression has type \"" + type_name(value_type) +
              "\", " + target_label + " has type \"" + type_name(target_type) + "\")");
}

} // namespace cythonpp::domain::semantic
