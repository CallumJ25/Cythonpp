#include "expression_typer.h"

#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "builtin_call_table.h"
#include "domain/ast/source_span.h"
#include "domain/lexer/keyword_table.h"
#include "domain/lexer/operator_table.h"
#include "domain/lexer/token_type.h"
#include "literal_type.h"
#include "operator_rules.h"
#include "type_compatibility.h"
#include "type_name.h"

namespace cythonpp::domain::semantic {
namespace {

// An operator's source spelling, for diagnostics. Punctuation-spelled
// operators (`+`, `<`, ...) live in operator_table.cpp; the reserved-word
// operators (`and`, `or`, `not`, `is`, `in`, and the parser-synthesised `is
// not` / `not in`) live in keyword_table.cpp. Trying the first and falling
// back to the second covers every token_type ast::BinOp, ast::UnaryOp,
// ast::Compare and ast::BoolOp can carry.
std::string operator_symbol(lexer::token_type op) {
    const std::string_view punctuation = lexer::operator_lexeme_of(op);
    if (!punctuation.empty()) {
        return std::string(punctuation);
    }
    return std::string(lexer::keyword_lexeme_of(op));
}

// "unsupported operand types for + (\"int\" and \"str\")" -- the wording this
// task settles on for a genuine (NotApplicable) operand-type error, shared by
// BinOp and each Compare chain link so the corpus sees one spelling rather
// than two. Recorded here because a later task's corpus matches it verbatim.
std::string operand_type_error_message(lexer::token_type op, const Type& left, const Type& right) {
    return "unsupported operand types for " + operator_symbol(op) + " (\"" + type_name(left) +
           "\" and \"" + type_name(right) + "\")";
}

// "methods on builtin types are not supported" -- reported for BOTH a
// directly builtin receiver (xs.append, s.upper, d.keys -- no typeshed here
// to consult) and, via the carve-out below, a user class whose base chain
// reaches a builtin kind when the member is not one WE know about. One
// constant so the two call sites cannot drift apart.
const char* kBuiltinMemberMessage = "methods on builtin types are not supported";

// Only the five container constructors -- list, dict, set, frozenset, tuple
// -- follow the empty-display rule; every other supported builtin's nullopt
// from builtin_call_result is a genuine mismatch, which IS reported. A tiny
// local helper rather than duplicating is_empty_display_builtin's own name
// list, so the two call sites cannot drift apart.
bool is_silent_when_argumentless(const std::string& name, bool has_arguments) {
    return !has_arguments && is_empty_display_builtin(name);
}

} // namespace

ExpressionTyper::ExpressionTyper(ScopeStack& scopes, const ClassTable& classes, TypeMap& types,
                                 diagnostics::DiagnosticSink& sink)
    : scopes_(scopes), classes_(classes), types_(types), sink_(sink) {}

Type ExpressionTyper::type_of(const ast::Expr& expr, const Type& expected) {
    Type result = Type::unknown();
    if (const auto* constant = dynamic_cast<const ast::Constant*>(&expr)) {
        result = type_of_constant(*constant, /*negated=*/false);
    } else if (const auto* name = dynamic_cast<const ast::Name*>(&expr)) {
        result = type_of_name(*name);
    } else if (const auto* unary = dynamic_cast<const ast::UnaryOp*>(&expr)) {
        result = type_of_unary_op(*unary);
    } else if (const auto* bin_op = dynamic_cast<const ast::BinOp*>(&expr)) {
        result = type_of_bin_op(*bin_op);
    } else if (const auto* compare = dynamic_cast<const ast::Compare*>(&expr)) {
        result = type_of_compare(*compare);
    } else if (const auto* bool_op = dynamic_cast<const ast::BoolOp*>(&expr)) {
        result = type_of_bool_op(*bool_op);
    } else if (const auto* list = dynamic_cast<const ast::ListExpr*>(&expr)) {
        result = type_of_list(*list, expected);
    } else if (const auto* dict = dynamic_cast<const ast::DictExpr*>(&expr)) {
        result = type_of_dict(*dict, expected);
    } else if (const auto* tuple = dynamic_cast<const ast::TupleExpr*>(&expr)) {
        result = type_of_tuple(*tuple, expected);
    } else if (const auto* subscript = dynamic_cast<const ast::Subscript*>(&expr)) {
        result = type_of_subscript(*subscript);
    } else if (const auto* attribute = dynamic_cast<const ast::Attribute*>(&expr)) {
        result = type_of_attribute(*attribute);
    } else if (const auto* call = dynamic_cast<const ast::Call*>(&expr)) {
        result = type_of_call(*call, expected);
    } else {
        // Every other Expr kind is a later task's arm (ListComp: 16, ...).
        // Deliberately SILENT -- not error() -- so an intermediate build
        // never emits a diagnostic a later task has to un-emit.
        result = Type::unknown();
    }
    // Every expression the typer types gets an entry, unconditionally --
    // including this silent fallback, since type_of() was still asked to
    // type it. Annotation subtrees never reach here at all: AnnotationResolver
    // walks those directly and type_of() is never called on one.
    types_.insert(&expr, result);
    return result;
}

Type ExpressionTyper::type_of_constant(const ast::Constant& constant, bool negated) {
    if (constant.type() == lexer::token_type::LITERAL_INT &&
        !integer_literal_fits_64_bits(constant.lexeme(), /*allow_two_to_63=*/negated)) {
        return error(constant, "OverflowError",
                     "integer literal is too large for a 64-bit integer");
    }
    return literal_type(constant.type());
}

Type ExpressionTyper::type_of_name(const ast::Name& name) {
    const Resolution resolution = scopes_.resolve(name.identifier());
    if (resolution.binding == nullptr) {
        return error(name, "NameError", "name '" + name.identifier() + "' is not defined");
    }
    return resolution.binding->type;
}

Type ExpressionTyper::type_of_unary_op(const ast::UnaryOp& unary) {
    const ast::Expr& operand_expr = unary.operand();
    const auto* operand_constant = dynamic_cast<const ast::Constant*>(&operand_expr);

    Type operand_type;
    if (unary.op() == lexer::token_type::OP_MINUS && operand_constant != nullptr &&
        operand_constant->type() == lexer::token_type::LITERAL_INT) {
        // The sign is ours to apply: this is the one case where a magnitude
        // of exactly 2^63 is representable (-2^63), so type_of() -- which
        // would check the non-negated, strictly-less-than-2^63 rule -- must
        // NOT be called on this operand. type_of() is bypassed entirely, so
        // the map entry it would have written is written by hand instead.
        operand_type = type_of_constant(*operand_constant, /*negated=*/true);
        types_.insert(&operand_expr, operand_type);
    } else {
        operand_type = type_of(operand_expr, Type::unknown());
    }

    const RuleResult result = unary_result(unary.op(), operand_type);
    return apply(result, unary,
                 "unsupported operand type for unary " + operator_symbol(unary.op()) + " (\"" +
                     type_name(operand_type) + "\")");
}

Type ExpressionTyper::type_of_bin_op(const ast::BinOp& bin_op) {
    const Type left = type_of(bin_op.left(), Type::unknown());
    const Type right = type_of(bin_op.right(), Type::unknown());
    const RuleResult result = binary_result(bin_op.op(), left, right);
    return apply(result, bin_op, operand_type_error_message(bin_op.op(), left, right));
}

Type ExpressionTyper::type_of_compare(const ast::Compare& compare) {
    // A chain of n links: type left() once, then each Rest::operand once, and
    // call comparison_result per link. comparison_result never returns
    // Unknown, so the chain's type is ALWAYS Bool -- even when a link
    // failed -- and each failing link reports exactly once, through apply();
    // the chain itself adds nothing.
    //
    // DELIBERATE DIVERGENCE from type_of_bin_op: apply() is called here with
    // `*link.operand` -- the failing link's RIGHT-HAND operand -- not
    // `compare` (the whole chain), whereas type_of_bin_op reports at `bin_op`
    // (the whole expression). A BinOp has exactly one operator, so its own
    // span is the only useful anchor. A Compare chain can have several
    // links, each its own root cause (see EachFailingChainLinkIsItsOwnRootCause
    // below), and anchoring every failure at the chain's start would collapse
    // two distinct failures onto one column. So `1 < "s"` reports at column 5
    // (the `"s"`) while `1 + "s"` reports at column 1 (the whole `1 + "s"`).
    // Pinned by BinOpReportsAtTheWholeExpressionButCompareReportsAtTheFailingOperand.
    Type previous = type_of(compare.left(), Type::unknown());
    for (const ast::Compare::Rest& link : compare.rest()) {
        const Type operand = type_of(*link.operand, Type::unknown());
        const RuleResult result = comparison_result(link.op, previous, operand);
        apply(result, *link.operand, operand_type_error_message(link.op, previous, operand));
        previous = operand;
    }
    return Type::bool_();
}

Type ExpressionTyper::type_of_bool_op(const ast::BoolOp& bool_op) {
    std::vector<Type> operand_types;
    operand_types.reserve(bool_op.values().size());
    for (const ast::ExprPtr& value : bool_op.values()) {
        operand_types.push_back(type_of(*value, Type::unknown()));
    }
    const RuleResult result = boolop_result(bool_op.op(), operand_types);
    // Unreachable via a real parse (the parser only ever builds a BoolOp with
    // OP_AND/OP_OR and at least two values), so this wording is never
    // exercised by a mypy-clean program; it exists only so apply()'s
    // NotApplicable arm has something to say.
    return apply(result, bool_op, "unsupported operand types for " + operator_symbol(bool_op.op()));
}

Type ExpressionTyper::type_of_list(const ast::ListExpr& list, const Type& expected) {
    const std::vector<ast::ExprPtr>& elements = list.elements();

    // "Matching kind and the right argument count": List always carries
    // exactly one element type via list_of, so the count check is really
    // just defending against a hand-built Type; it can never fail for one
    // produced by AnnotationResolver.
    const bool has_context = expected.kind == TypeKind::List && expected.args.size() == 1;

    if (has_context) {
        const Type& element_type = expected.args[0];
        for (std::size_t index = 0; index < elements.size(); ++index) {
            const Type actual = type_of(*elements[index], element_type);
            if (!is_subtype(actual, element_type, &classes_)) {
                error(*elements[index], "TypeError",
                     "list item " + std::to_string(index) + " has incompatible type \"" +
                         type_name(actual) + "\"; expected \"" + type_name(element_type) + "\"");
            }
        }
        // Declared, not joined -- even a bad element does not change the
        // list's own reported type, matching mypy: the error is about the
        // element, not the container.
        return expected;
    }

    // No usable context: fold every element's own inferred type through
    // join(). An empty display has no element to seed the fold with and no
    // annotation to fall back on, so it is silently Unknown -- reporting here
    // would be a second diagnostic for the same root cause a later task's
    // Assign/AnnAssign arm already owns.
    if (elements.empty()) {
        return Type::unknown();
    }
    Type joined = type_of(*elements.front(), Type::unknown());
    for (std::size_t index = 1; index < elements.size(); ++index) {
        joined = join(joined, type_of(*elements[index], Type::unknown()), &classes_);
    }
    return Type::list_of(std::move(joined));
}

Type ExpressionTyper::type_of_dict(const ast::DictExpr& dict, const Type& expected) {
    const std::vector<ast::DictExpr::Entry>& entries = dict.entries();
    const bool has_context = expected.kind == TypeKind::Dict && expected.args.size() == 2;

    if (has_context) {
        const Type& key_expected = expected.args[0];
        const Type& value_expected = expected.args[1];
        for (std::size_t index = 0; index < entries.size(); ++index) {
            const Type actual_key = type_of(*entries[index].key, key_expected);
            const Type actual_value = type_of(*entries[index].value, value_expected);
            const bool key_ok = is_subtype(actual_key, key_expected, &classes_);
            const bool value_ok = is_subtype(actual_value, value_expected, &classes_);
            // One diagnostic per bad entry, not one per bad half -- mypy
            // reports the whole "key: value" pair together even when only
            // one side is wrong (see the class comment's example, where the
            // value side ("int": "int") matches but is still quoted).
            if (!key_ok || !value_ok) {
                error(*entries[index].key, "TypeError",
                     "dict entry " + std::to_string(index) + " has incompatible type \"" +
                         type_name(actual_key) + "\": \"" + type_name(actual_value) +
                         "\"; expected \"" + type_name(key_expected) + "\": \"" +
                         type_name(value_expected) + "\"");
            }
        }
        return expected;
    }

    if (entries.empty()) {
        return Type::unknown();
    }
    Type joined_key = type_of(*entries.front().key, Type::unknown());
    Type joined_value = type_of(*entries.front().value, Type::unknown());
    for (std::size_t index = 1; index < entries.size(); ++index) {
        joined_key = join(joined_key, type_of(*entries[index].key, Type::unknown()), &classes_);
        joined_value =
            join(joined_value, type_of(*entries[index].value, Type::unknown()), &classes_);
    }
    return Type::dict_of(std::move(joined_key), std::move(joined_value));
}

Type ExpressionTyper::type_of_tuple(const ast::TupleExpr& tuple, const Type& expected) {
    const std::vector<ast::ExprPtr>& elements = tuple.elements();

    // Unlike List/Dict, a Tuple's arity is genuinely variable, so "the right
    // argument count" is a real condition here, not a defensive no-op: a
    // context of a DIFFERENT arity is the wrong shape (x: tuple[int] =
    // (1, 2)) and must fall back to plain inference rather than being
    // adopted, exactly as AContextOfTheWrongShapeFallsBackToInference does
    // for List.
    const bool has_context =
        expected.kind == TypeKind::Tuple && expected.args.size() == elements.size();

    // No join branch at all: a tuple's elements are positional, not
    // homogeneous, so the result is always built from each element's own
    // inferred type -- WITH a matching context, `element_expected` still
    // propagates into the recursive type_of() call (so e.g. a nested `[]`
    // resolves against its declared element type instead of going Unknown),
    // but the result is NEVER checked against it here: mypy has no per-item
    // tuple diagnostic. Both an element mismatch (`x: tuple[int, str] =
    // (1, 2)`) and an arity mismatch (`x: tuple[int, str] = (1,)`) surface as
    // ONE `assignment` error naming the two whole tuple types -- which the
    // later assignment check produces for free from the POSITIONAL type
    // returned below, since is_subtype's Tuple arm is covariant, elementwise,
    // and arity-checked. Reporting here too would be a second diagnostic for
    // the same root cause.
    std::vector<Type> element_types;
    element_types.reserve(elements.size());
    for (std::size_t index = 0; index < elements.size(); ++index) {
        const Type element_expected = has_context ? expected.args[index] : Type::unknown();
        element_types.push_back(type_of(*elements[index], element_expected));
    }
    // Not `expected`, even when has_context: unlike List/Dict, Tuple has no
    // single "declared element type" to fall back on for a bad position, so
    // the tuple's own positional element types -- not the declared ones --
    // are what carries into the result, matching every other display's own
    // elements always driving the answer here.
    return Type::tuple_of(std::move(element_types));
}

Type ExpressionTyper::type_of_subscript(const ast::Subscript& subscript) {
    const Type container = type_of(subscript.value(), Type::unknown());
    const Type index = type_of(subscript.index(), Type::unknown());
    const RuleResult result = subscript_result(container, index, &classes_);
    return apply(result, subscript,
                 "invalid index type \"" + type_name(index) + "\" for \"" + type_name(container) +
                     "\"");
}

Type ExpressionTyper::type_of_attribute(const ast::Attribute& attribute) {
    // CLASS-OBJECT RECEIVER, checked syntactically and BEFORE the receiver
    // is ever typed: `C.x` / `C.m`. Verified against mypy 1.18.1: both are
    // mypy-clean (`reveal_type(C.x)` is `builtins.int`, `reveal_type(C.m)` is
    // `def (self: C) -> int`). `C` is a bare Name that nothing binds into
    // ScopeStack -- typing it through the ordinary type_of()/type_of_name()
    // path below would report a false NameError. A qualified nested-class
    // receiver (`Outer.Inner.x`) is out of scope: its own receiver is an
    // Attribute, not a Name, so it falls through to the ordinary path below
    // and reports (rather than guesses) once it gets there.
    //
    // PRECEDENCE, checked here to fix a shadowing bug from fix round 1: a
    // local binding of the SAME name as a class must win over the
    // class-object reading. `def f(Widget: int): return Widget.bit_length()`
    // is legal Python where `Widget` is an int parameter, not the class --
    // scopes_.resolve() is consulted FIRST, and the class-object path is
    // only taken when it comes back null. This is correct only as long as
    // class names are never themselves bound into ScopeStack. Today the
    // statement checker (a later task) declares classes into ClassTable
    // without also binding their names as scope values, which is exactly
    // why the class-object path exists at all. If a later task starts
    // binding class names into ScopeStack (e.g. so a class can be passed
    // around as a first-class value), this precedence check silently
    // inverts: scopes_.resolve() would then find the class's own binding
    // and this whole branch would never fire, turning every `C.x` into
    // whatever the ordinary value path does with a class-valued binding.
    // Revisit this check at that point.
    if (const auto* receiver_name = dynamic_cast<const ast::Name*>(&attribute.value())) {
        if (classes_.is_class(receiver_name->identifier()) &&
            scopes_.resolve(receiver_name->identifier()).binding == nullptr) {
            const Type class_type = Type::class_of(receiver_name->identifier());
            // Recorded by hand, not through type_of(), since type_of_name()
            // -- which would consult ScopeStack and report NameError -- is
            // exactly the path being avoided here.
            types_.insert(receiver_name, class_type);
            return type_of_class_attribute(class_type, attribute, /*bind_self=*/false);
        }
    }

    const Type receiver = type_of(attribute.value(), Type::unknown());
    switch (receiver.kind) {
    case TypeKind::Unknown:
        // The root cause (an unbound name, a prior failed subexpression, ...)
        // already reported. Absorbing, like every other arm's Unknown input.
        return Type::unknown();
    case TypeKind::Union:
        // mypy narrows; this compiler does not model per-branch environments
        // yet, so `(A | None).f` is deferred rather than guessed at, exactly
        // like every other union-operand case.
        return error(attribute, "NotImplementedError",
                     unsupported_message(UnsupportedReason::UnionOperand));
    case TypeKind::Class:
        // An INSTANCE receiver: self is bound (dropped) below, per THE self
        // CONTRACT -- see type_of_class_attribute's declaration comment.
        return type_of_class_attribute(receiver, attribute, /*bind_self=*/true);
    // Every builtin kind: no typeshed is consulted here, so a member access
    // on any of these is unmodellable rather than a guess -- reporting
    // attr-defined without knowing str/list/dict's real members would be a
    // false TypeError on some of the most common lines in Python
    // (xs.append(1), s.upper(), d.keys()).
    case TypeKind::NoneType:
    case TypeKind::Bool:
    case TypeKind::Int:
    case TypeKind::Float:
    case TypeKind::Complex:
    case TypeKind::Str:
    case TypeKind::Bytes:
    case TypeKind::ByteArray:
    case TypeKind::Ellipsis:
    case TypeKind::List:
    case TypeKind::Dict:
    case TypeKind::Set:
    case TypeKind::FrozenSet:
    case TypeKind::Tuple:
    case TypeKind::Range:
    case TypeKind::Callable:
    case TypeKind::Object:
        return error(attribute, "NotImplementedError", kBuiltinMemberMessage);
    }
    // Unreachable: the switch is exhaustive over TypeKind and has no default,
    // so adding a kind warns here rather than silently falling through.
    return Type::unknown();
}

Type ExpressionTyper::type_of_class_attribute(const Type& receiver, const ast::Attribute& attribute,
                                              bool bind_self) {
    // member_type and method_type are TWO SEPARATE ClassTable queries --
    // declare_member writes into Entry::members, declare_method into
    // Entry::methods, and each query only ever searches its own map. Trying
    // member_type alone would make every ordinary method reference (`c.m`)
    // a false attr-defined TypeError, which is the hard invariant. Member is
    // checked first, and WITHIN one class that ordering is not load-bearing
    // (a real class never declares the same name as both a member and a
    // method). It IS load-bearing across a base chain, though: member_type
    // walks the WHOLE chain before method_type is ever tried, so a base's
    // attribute beats a derived class's method of the same name. Harmless in
    // practice -- mypy itself rejects that shape of override, so the program
    // is not clean anyway -- but the guarantee only holds at that strength,
    // not "no class can have the same name in both" as a prior version of
    // this comment overclaimed.
    if (const std::optional<Type> member =
            classes_.member_type(receiver.name, attribute.attribute())) {
        return *member;
    }
    if (const std::optional<Type> method =
            classes_.method_type(receiver.name, attribute.attribute())) {
        if (!bind_self) {
            // CLASS-OBJECT receiver (`C.m`): returned AS-IS, self included.
            // Verified against mypy 1.18.1: reveal_type(C.m) is
            // `def (self: C) -> int`.
            return *method;
        }
        // INSTANCE receiver (`c.m`): self is dropped HERE, not by a later
        // Call arm. Verified against mypy 1.18.1: reveal_type(c.m) is
        // `def () -> int` -- mypy binds self at the attribute access itself.
        // method->args is [self, param..., return] (Type::callable's
        // convention, return LAST), so erasing args[0] is exactly "bind
        // self". THE self CONTRACT for Task 15: a Call arm whose callee is
        // this Attribute must NOT drop args[0] again -- it is already bound.
        Type bound = *method;
        bound.args.erase(bound.args.begin());
        return bound;
    }
    // A class may define __getattr__ to make ARBITRARY attribute access
    // clean. Verified against mypy 1.18.1: `class G: def __getattr__(self,
    // name: str) -> int: ...` then `g.anything` is mypy-CLEAN, revealing
    // `builtins.int`. Checked before concluding a miss -- otherwise this
    // would be a false TypeError on mypy-clean code, the hard invariant.
    //
    // Resolved EXACTLY here, unlike the operator dunders (__add__, __eq__,
    // ...), which remain DEFERRED for a later task: operator dispatch needs
    // ~30 names with reflected fallbacks and fiddly exceptions (`in` is
    // always `bool` regardless of what `__contains__` declares), whereas
    // __getattr__ is a single lookup with no binding subtlety and no
    // reflected form. The asymmetry between the two is deliberate, not an
    // oversight.
    //
    // __getattr__'s own args are [self, name: str, return], return LAST, so
    // args.back() is its declared return type -- exactly what every access
    // it resolves reveals as.
    if (const std::optional<Type> getattr =
            classes_.method_type(receiver.name, "__getattr__")) {
        return getattr->args.back();
    }
    // THE CARVE-OUT: `class Sub(int): pass` then `Sub().bit_length()` is
    // mypy-clean, because Sub inherits int's members, which this compiler
    // does not model (no typeshed). Without this row, an attribute miss on
    // Sub would be a false TypeError. The accepted cost is a MISSED error:
    // `Sub().nope` is a genuine mypy attr-defined error and this reports
    // NotImplementedError instead -- direction (b) loses attr-defined for
    // builtin-inheriting classes only, and the hard invariant (never a false
    // TypeError) holds.
    if (classes_.inherits_builtin(receiver.name)) {
        return error(attribute, "NotImplementedError", kBuiltinMemberMessage);
    }
    // Every base is an ordinary user-class-shaped entry (including a seeded
    // exception class -- inherits_builtin is deliberately false for those),
    // so a miss here is a genuine mypy attr-defined error.
    return error(attribute, "TypeError",
                 "\"" + receiver.name + "\" has no attribute \"" + attribute.attribute() + "\"");
}

Type ExpressionTyper::type_of_call(const ast::Call& call, const Type& expected) {
    if (const auto* callee_name = dynamic_cast<const ast::Name*>(&call.callee())) {
        return type_of_name_call(*callee_name, call, expected);
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
                label += " of \"" + receiver_type->name + "\"";
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
        if (is_silent_when_argumentless(identifier, !arg_types.empty())) {
            // The empty-display rule: a bare list()/dict()/set()/
            // frozenset()/tuple() with no usable context is silently
            // Unknown, exactly like []/{} -- the var-annotated error belongs
            // to a later task's Assign arm, which alone has the variable's
            // name to put in it.
            return Type::unknown();
        }
        // Every other supported builtin's nullopt is a genuine, modelled
        // mismatch (wrong arity or argument kind). Type::callable carries no
        // parameter NAMES, so this cannot reproduce mypy's real per-overload
        // wording; it names the builtin instead.
        return error(call, "TypeError",
                     "argument has incompatible type for \"" + identifier + "\"");
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
    // callable.args is [param..., return], return LAST (Type::callable's
    // convention), so it is never empty and param_count is args.size() - 1.
    const std::size_t param_count = callable.args.size() - 1;
    const Type& return_type = callable.args.back();

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
    if (arg_exprs.size() < param_count) {
        const std::size_t missing = param_count - arg_exprs.size();
        // mypy names the missing PARAMETER ('Missing positional argument "b"
        // in call to "f"'); Type carries no parameter names (see type.h --
        // Callable's args are types only), so this reports the COUNT
        // instead. A recorded, deliberate divergence from the brief's exact
        // wording -- see the task report.
        return error(call, "TypeError",
                     "missing " + std::to_string(missing) + " positional argument" +
                         (missing == 1 ? "" : "s") + " in call to " + label);
    }

    // Arity matches exactly: each argument is checked against its own
    // parameter type, is_subtype (never operator==) so a subclass or a
    // wider numeric rank is accepted. A mismatch is its own diagnostic --
    // N bad arguments is N diagnostics, matching the per-item list/dict
    // rule -- numbered from the FIRST USER argument; self is never counted,
    // since it was already dropped (or never added) before `callable`
    // reached this function.
    for (std::size_t index = 0; index < param_count; ++index) {
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

Type ExpressionTyper::apply(const RuleResult& result, const ast::Expr& at,
                            std::string type_error_message) {
    switch (result.status) {
    case RuleResult::Status::Ok:
        return result.type;
    case RuleResult::Status::NotApplicable:
        return error(at, "TypeError", std::move(type_error_message));
    case RuleResult::Status::Unsupported:
        return error(at, "NotImplementedError", unsupported_message(result.reason));
    }
    // Unreachable: the switch is exhaustive over RuleResult::Status and has no
    // default, so adding a status warns here rather than silently falling
    // through to a false Ok or a false TypeError.
    return Type::unknown();
}

Type ExpressionTyper::error(const ast::Expr& at, std::string code, std::string message) {
    const ast::SourceSpan span = at.span();
    sink_.report_error(std::move(code), std::move(message), span.start_line, span.start_column);
    return Type::unknown();
}

} // namespace cythonpp::domain::semantic
