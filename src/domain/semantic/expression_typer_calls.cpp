// A second translation unit for ExpressionTyper (see expression_typer.h,
// which is NOT split -- declarations and their comments stay together there,
// so the self CONTRACT keeps living next to type_of_class_attribute, which
// establishes it). This file holds only the Call arm (Task 15):
// type_of_call, type_of_name_call, type_of_positional_call and
// type_of_call_result, plus their one file-local helper -- a self-contained
// 224-line block that references no other file-local helper in
// expression_typer.cpp, not even apply(), which is what makes this a real
// seam rather than an arbitrary cut. Split out in fix round 1 because
// expression_typer.cpp had grown to 732 lines and Task 16's ListComp arm
// still had to land in it.
#include "expression_typer.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "builtin_call_table.h"
#include "type_compatibility.h"
#include "type_name.h"

namespace cythonpp::domain::semantic {
namespace {

// Only the five container constructors -- list, dict, set, frozenset, tuple
// -- follow the empty-display rule, and only when called with ZERO
// arguments; every other nullopt from builtin_call_result -- including one of
// these five names called with a non-empty argument list -- is an unmodelled
// shape, handled by the caller's fallback below. A tiny local helper rather
// than duplicating is_empty_display_builtin's own name list, so the two call
// sites cannot drift apart.
//
// Fix round 1, Finding 4: renamed from is_silent_when_argumentless(name,
// has_arguments) -- a predicate named "argumentless" whose second parameter
// was has_arguments, called with !arg_types.empty(), which read backwards at
// the call site. Taking the count directly reads straight: zero arguments,
// not "not has_arguments".
bool is_bare_container_constructor(const std::string& name, std::size_t arg_count) {
    return arg_count == 0 && is_empty_display_builtin(name);
}

} // namespace

Type ExpressionTyper::type_of_call(const ast::Call& call, const Type& expected) {
    if (const auto* callee_name = dynamic_cast<const ast::Name*>(&call.callee())) {
        return type_of_name_call(*callee_name, call, expected);
    }

    // NESTED-CLASS CONSTRUCTOR (Task 19): `Outer.Inner()`. Checked
    // syntactically, BEFORE the callee is typed at all -- mirroring
    // type_of_name_call's own is_class(identifier) branch for a bare `C()`,
    // which exists for the identical reason: Type has no way to distinguish
    // "a Class-kind VALUE" (an instance, which type_of_call_result's Class
    // branch correctly treats as an unmodelled __call__) from "a class
    // OBJECT reference" (which must construct). type_of_attribute's own
    // class-object-receiver check (see its declaration comment) only
    // recognises a bare-Name receiver, so a two-segment chain like this one
    // is never routed there; going through the ordinary type_of(call.
    // callee(), ...) dispatch below would fall into type_of_attribute's
    // general Class-receiver arm instead, which looks ONLY at members and
    // methods and has no notion of a nested class -- a false attr-defined
    // TypeError on mypy-clean code. `root`'s own scope binding must still
    // win, matching the precedence hazard documented on
    // type_of_attribute's class-object check.
    if (const auto* attribute = dynamic_cast<const ast::Attribute*>(&call.callee())) {
        if (const auto* root = dynamic_cast<const ast::Name*>(&attribute->value())) {
            const std::string dotted = root->identifier() + "." + attribute->attribute();
            if (classes_.is_class(dotted) && scopes_.resolve(root->identifier()).binding == nullptr) {
                // Recorded by hand, not through type_of(): typing `root`
                // through the ordinary type_of_name() path would report a
                // false NameError, exactly as every other class-object
                // reference in this file explains.
                types_.insert(root, Type::class_of(root->identifier()));
                const Type constructor = classes_.constructor_type(dotted);
                // The Attribute node itself gets the CONSTRUCTOR's type, not
                // Class(dotted) -- matching type_of_name_call's identical
                // choice for a bare `C` used as a callee (reveal_type(C) is
                // `def (...) -> C`, not `type[C]`).
                types_.insert(attribute, constructor);
                return type_of_positional_call(constructor, call, "\"" + dotted + "\"");
            }
        }
    }

    // Attribute and every other callee shape: type the callee through the
    // ordinary dispatcher (for an Attribute this runs type_of_attribute,
    // which already applies THE self CONTRACT -- an instance method arrives
    // here already bound, a class-object one deliberately unbound -- so
    // type_of_call_result below must NOT drop args[0] again).
    const Type callee_type = type_of(call.callee(), Type::unknown());

    if (const auto* attribute = dynamic_cast<const ast::Attribute*>(&call.callee())) {
        // The method-call naming shape, `"m" of "C"`: the receiver's type was
        // already recorded in `types_` by type_of_attribute -- either via its
        // own type_of() call on an ordinary (instance) receiver, or by hand
        // for a class-object receiver -- so it is read back here rather than
        // re-typing the receiver a second time.
        std::string label = "\"" + attribute->attribute() + "\"";
        if (const Type* receiver_type = types_.find(&attribute->value())) {
            if (receiver_type->kind == TypeKind::Class) {
                // Fix round 2 (Task 19 fix round 2, Finding C): stripped the
                // same way type_name's own Class case is -- see that
                // function's comment -- so a call-argument error on an
                // isolated class's method never quotes TypeChecker's
                // internal "<tag>#<line>#" disambiguator.
                label += " of \"" + strip_synthetic_class_prefix(receiver_type->name) + "\"";
            }
        }
        return type_of_call_result(callee_type, call, label);
    }

    // Any other callee shape the parser permits (e.g. `f()()`, a Call
    // callee): no name to quote, so the callee's own rendered type stands in
    // for the label. Untested corner -- nothing in this task's corpus
    // exercises it -- but total rather than unreachable.
    return type_of_call_result(callee_type, call, "\"" + type_name(callee_type) + "\"");
}

Type ExpressionTyper::type_of_name_call(const ast::Name& callee, const ast::Call& call,
                                        const Type& expected) {
    const std::string& identifier = callee.identifier();

    // PRECEDENCE: a live scope binding wins first. The brief's own wording
    // ("then a user class, then a builtin") turns out to invert against
    // builtin_class_table.h's own documented invariant once seeded names are
    // considered: "range", "int", "str", "list", "zip" and friends are ALL
    // present in ClassTable (see builtin_class_table.h's "Names that ARE
    // model kinds... appear here too" and its generic-class carve-out for
    // zip/map/filter/enumerate/reversed), because that table is a generated
    // extraction of every class in Python's builtins module, not a
    // hand-curated "user classes only" list. Every other consumer in this
    // codebase (annotation_resolver.cpp) already checks the builtin
    // classifier BEFORE ClassLookup::is_class() for exactly this reason;
    // checking is_class() first here made `range(3)`, `int("5")` and
    // `zip(...)` all resolve as zero-arg-constructor CLASSES instead of
    // builtin calls, verified by this task's own test failures. So the
    // builtin check runs first, and classes_.is_class() only ever sees a
    // name that is NOT one of this project's known builtin call names --
    // genuine user classes and seeded exception classes (ValueError, ...),
    // which are not in that set at all.
    if (scopes_.resolve(identifier).binding != nullptr) {
        const Type callee_type = type_of(callee, Type::unknown());
        return type_of_call_result(callee_type, call, "\"" + identifier + "\"");
    }

    if (is_supported_builtin_call(identifier)) {
        // No modelled "value" type for a builtin function itself (unlike a
        // user Callable, there is no signature Type to hand back for `print`
        // on its own) -- Unknown is recorded so the callee expression still
        // gets a TypeMap entry, matching every other expression.
        types_.insert(&callee, Type::unknown());

        std::vector<Type> arg_types;
        arg_types.reserve(call.args().size());
        for (const ast::ExprPtr& arg : call.args()) {
            arg_types.push_back(type_of(*arg, Type::unknown()));
        }

        if (const std::optional<Type> result =
                builtin_call_result(identifier, arg_types, expected)) {
            return *result;
        }
        if (is_bare_container_constructor(identifier, arg_types.size())) {
            // The empty-display rule: a bare list()/dict()/set()/
            // frozenset()/tuple() with no usable context is silently
            // Unknown, exactly like []/{} -- the var-annotated error belongs
            // to a later task's Assign arm, which alone has the variable's
            // name to put in it.
            return Type::unknown();
        }
        // Fix round 1, Finding 1 (CRITICAL): a nullopt from
        // builtin_call_result for a SUPPORTED name cannot distinguish "mypy
        // would reject this" from "this shape is simply not modelled yet".
        // `list(range(3))`, `round(x, 2)`, `int("ff", 16)`,
        // `divmod(7.0, 2.0)`, and `len(w)`/`abs(w)`/`sorted(w)` on a user
        // class defining the matching dunder are ALL mypy-clean today and
        // would have drawn a false TypeError here -- `list(range(3))` is
        // about as common as Python gets. Reporting NotImplementedError
        // instead is the same direction-(b) choice every other unmodellable
        // shape in this file takes (see type_of_call_result's Union/Class
        // branches, or the unsupported-builtin-name branch just below).
        // Type::callable carries no parameter NAMES and builtin_call_table
        // has no per-overload diagnostic text, so this cannot reproduce
        // mypy's real wording; it names the builtin instead.
        return error(call, "NotImplementedError",
                     "calls to builtin '" + identifier +
                         "' with these argument types are not supported");
    }

    if (is_builtin_callable_name(identifier)) {
        types_.insert(&callee, Type::unknown());
        for (const ast::ExprPtr& arg : call.args()) {
            type_of(*arg, Type::unknown());
        }
        // NEVER NameError: the name IS defined and mypy accepts the call, so
        // NameError would be a false positive against the hard invariant.
        return error(call, "NotImplementedError",
                     "calls to builtin '" + identifier + "' are not supported");
    }

    if (classes_.is_class(identifier)) {
        // The constructor. constructor_type already walks the base chain for
        // an inherited __init__ and already strips self, so callable's args
        // here are exactly the caller-supplied parameters, return LAST.
        // Recorded by hand, not through type_of(): nothing binds a class's
        // own name into ScopeStack, so type_of_name() would report a false
        // NameError, exactly as type_of_attribute's class-object receiver
        // comment explains.
        const Type constructor = classes_.constructor_type(identifier);
        types_.insert(&callee, constructor);
        return type_of_positional_call(constructor, call, "\"" + identifier + "\"");
    }

    // An ordinary unbound name. Routed through type_of() (which dispatches
    // to type_of_name()) so the wording, position and TypeMap entry match
    // every other unbound-name path exactly, rather than duplicating them
    // here.
    const Type callee_type = type_of(callee, Type::unknown());
    for (const ast::ExprPtr& arg : call.args()) {
        type_of(*arg, Type::unknown());
    }
    return callee_type;
}

Type ExpressionTyper::type_of_positional_call(const Type& callable, const ast::Call& call,
                                              const std::string& label) {
    const std::vector<ast::ExprPtr>& arg_exprs = call.args();
    // Fix round 1, Finding 3: callable.args is [param..., return], return
    // LAST (Type::callable's convention), so in practice it is never empty --
    // Type::callable always pushes the return, and constructor_type/
    // bind_self's self-drop only ever removes ONE element from an args list
    // that started with at least [self, return]. Unreachable TODAY, but not
    // provably so from this function's own signature alone (a method
    // declared without `self` would have its return erased by the
    // bind_self drop instead, leaving args empty), so guarded rather than
    // trusted: callable.args.size() - 1 on an empty vector would underflow to
    // SIZE_MAX and callable.args.back() below would be UB.
    if (callable.args.empty()) {
        return Type::unknown();
    }
    const std::size_t param_count = callable.args.size() - 1;
    const Type& return_type = callable.args.back();
    // How many arguments the caller MUST supply. A defaulted parameter may be
    // omitted -- `def log(msg: str, level: int = 1) -> None` accepts
    // `log("start")`, which mypy 1.18.1 confirms is clean, and which this
    // check used to reject as "too few arguments" because Type::callable
    // carried no notion of an optional parameter at all. std::min rather than
    // a bare subtraction: defaulted_params is a plain count and nothing in
    // Type's own invariants bounds it by the parameter count, so an
    // out-of-range value must clamp rather than underflow to SIZE_MAX and
    // make every call "too few".
    const std::size_t required_count = param_count - std::min(param_count, callable.defaulted_params);

    // Every argument is typed FIRST, unconditionally -- including an extra
    // one past param_count, which gets Type::unknown() as its own context --
    // so a root cause inside any argument (an unbound name, say) is reported
    // exactly once regardless of whether the call's arity is even right.
    std::vector<Type> arg_types;
    arg_types.reserve(arg_exprs.size());
    for (std::size_t index = 0; index < arg_exprs.size(); ++index) {
        const Type param_expected = index < param_count ? callable.args[index] : Type::unknown();
        arg_types.push_back(type_of(*arg_exprs[index], param_expected));
    }

    if (arg_exprs.size() > param_count) {
        return error(call, "TypeError", "too many arguments for " + label);
    }
    if (arg_exprs.size() < required_count) {
        // mypy names the missing
        // PARAMETER ('Missing positional argument "b" in call to "f"'), but
        // Type carries no parameter names (see type.h -- Callable's args are
        // types only), so there is no "b" to recover -- that constraint is
        // real. The substitute string is what was wrong: confirmed against
        // real mypy 1.18.1 that a callee with no recoverable parameter names
        // gets mypy's actual name-free spelling, "Too few arguments for
        // \"f\"" -- never an invented count form. Lower-cased to match this
        // file's own convention (see "too many arguments for" one branch
        // above), and symmetric with it for the same reason.
        return error(call, "TypeError", "too few arguments for " + label);
    }

    // Arity is now known good: between required_count and param_count
    // inclusive. Each SUPPLIED argument is checked against its own parameter
    // type, is_subtype (never operator==) so a subclass or a wider numeric
    // rank is accepted. A mismatch is its own diagnostic -- N bad arguments
    // is N diagnostics, matching the per-item list/dict rule -- numbered from
    // the FIRST USER argument; self is never counted, since it was already
    // dropped (or never added) before `callable` reached this function.
    //
    // The bound is arg_types.size(), NOT param_count: with a defaulted
    // parameter omitted the two differ, and indexing arg_types[index] up to
    // param_count would read past the end. An omitted parameter's default was
    // already checked against its annotation at the `def` itself
    // (type_checker.cpp's FunctionDef arm), so there is nothing to check for
    // it here.
    for (std::size_t index = 0; index < arg_types.size(); ++index) {
        const Type& expected_param = callable.args[index];
        if (!is_subtype(arg_types[index], expected_param, &classes_)) {
            error(*arg_exprs[index], "TypeError",
                 "argument " + std::to_string(index + 1) + " to " + label +
                     " has incompatible type \"" + type_name(arg_types[index]) + "\"; expected \"" +
                     type_name(expected_param) + "\"");
        }
    }
    return return_type;
}

Type ExpressionTyper::type_of_call_result(const Type& callee_type, const ast::Call& call,
                                          const std::string& label) {
    if (callee_type.kind == TypeKind::Callable) {
        return type_of_positional_call(callee_type, call, label);
    }

    // No parameter list to check arguments against, but every argument is
    // still typed against Type::unknown() -- consistent with every other
    // arm: a root cause inside one (an unbound name, say) must still report
    // exactly once.
    for (const ast::ExprPtr& arg : call.args()) {
        type_of(*arg, Type::unknown());
    }

    if (callee_type.kind == TypeKind::Unknown) {
        // Absorbing: the callee's own root cause (an unresolved name, a
        // prior failed subexpression, an already-reported attribute miss)
        // already reported.
        return Type::unknown();
    }
    if (callee_type.kind == TypeKind::Union) {
        // mypy narrows before deciding whether the call is even legal (a
        // `Callable[[], int] | None` callee needs narrowing first); this
        // compiler does not model per-branch environments yet, matching
        // every other operand arm's Union handling (see type_of_attribute,
        // binary_result, ...). Reporting TypeError here -- as the brief's
        // table implies by omission -- would risk a false positive on a
        // union whose every member is in fact callable.
        return error(call, "NotImplementedError",
                     unsupported_message(UnsupportedReason::UnionOperand));
    }
    if (callee_type.kind == TypeKind::Class) {
        // `__call__` may be user-defined; unmodelled, but NOT a genuine
        // TypeError -- verified mypy-clean when __call__ exists, so
        // reporting TypeError here would be a false positive against the
        // hard invariant.
        return error(call, "NotImplementedError",
                     "calling an instance of a user-defined class is not supported");
    }
    return error(call, "TypeError", "\"" + type_name(callee_type) + "\" not callable");
}

} // namespace cythonpp::domain::semantic
