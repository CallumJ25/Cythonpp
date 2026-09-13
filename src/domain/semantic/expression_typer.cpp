#include "expression_typer.h"

#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "builtin_call_table.h"
#include "builtin_type_names.h"
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

// RAII guard for the ONE expression (ListComp, Task 16) that pushes a scope.
// Several paths through that arm report and return early -- a non-iterable
// iterable, a tuple target -- and a bare pop() skipped by any one of them
// would corrupt every subsequent lookup in the file. Binding the pop to this
// guard's destructor means every return path is correct by construction,
// paired push/pop calls are not needed, and there is nothing to keep in
// sync when the arm grows another early return later.
class ComprehensionScopeGuard {
public:
    explicit ComprehensionScopeGuard(ScopeStack& scopes) : scopes_(scopes) {
        scopes_.push(ScopeKind::Comprehension);
    }
    ~ComprehensionScopeGuard() { scopes_.pop(); }

    ComprehensionScopeGuard(const ComprehensionScopeGuard&) = delete;
    ComprehensionScopeGuard& operator=(const ComprehensionScopeGuard&) = delete;

private:
    ScopeStack& scopes_;
};

// The integer VALUE of an index expression written as an integer literal --
// `t[0]`, `t[0x1]`, `t[1_0]`, `t[True]`, and `t[-1]`, which the AST
// represents as a UnaryOp(-) over a Constant, since the sign lives in the
// operator and never in the lexeme (the same split type_of_constant's own
// `negated` parameter exists for).
//
// WHICH FORMS, and why the list is this long rather than decimal-digits-only:
// mypy selects one tuple element whenever the index's own TYPE is a
// `Literal[n]`, and neither a literal's base, nor its digit separators, nor
// `bool` being an `int` subtype changes that. Measured, mypy 1.18.1, for
// `t: tuple[int, str]` unless noted:
//
//   reveal_type(t[0x1]) reveal_type(t[0X1]) reveal_type(t[0o1])
//   reveal_type(t[0b1]) reveal_type(t[0x_1]) reveal_type(t[True])
//                                            -> builtins.str
//   reveal_type(t[+0])  reveal_type(t[False]) -> builtins.int
//   t[1_0]                                    -> error: Tuple index out of
//                                                range  [misc]
//   on `t: tuple[int, str, float]`:
//   reveal_type(t[--1]) reveal_type(t[++1])   -> builtins.str
//   reveal_type(t[+-1]) reveal_type(t[-+1])   -> builtins.float
//
// So declining any of them is NOT free: handing back the union of every
// member is a FALSE TypeError the moment the result meets one member's type,
// and `s: str = t[0x1]` is accepted by mypy --strict AND prints `a` under
// CPython.
//
// std::nullopt for every other shape: a name, a call, an arithmetic
// expression, a non-integer literal, or a magnitude that does not fit 64
// bits. Two of the declines are measured decisions rather than gaps:
//
//   t[~0]      -- mypy reveals `builtins.int | builtins.str`. `~` yields a
//                 plain `int`, not a `Literal`, so the union genuinely IS
//                 mypy's answer here.
//   t[-True]   -- mypy reveals the whole union too (as do `t[+True]`,
//                 `t[-False]`, `t[+False]` and `t[--True]`, all measured on
//                 the three-element tuple). A bool is a literal index only
//                 while BARE; any unary operator over it erases the
//                 literalness. That is why the bool arm below runs BEFORE a
//                 single unary operator has been stripped.
std::optional<long long> literal_integer_index(const ast::Expr& index) {
    // 2^63, the magnitude bound: a negated index may reach exactly this
    // (-2^63 is representable), an unnegated one may not.
    constexpr unsigned long long TWO_TO_63 = 9223372036854775808ULL;

    // A BARE bool, before any unary stripping -- see the measurement above.
    if (const auto* boolean = dynamic_cast<const ast::Constant*>(&index)) {
        if (boolean->type() == lexer::token_type::BOOL_TRUE) {
            return 1;
        }
        if (boolean->type() == lexer::token_type::BOOL_FALSE) {
            return 0;
        }
    }

    // A CHAIN, not a single operator: `--1` is 1 and `+-1` is -1, and mypy
    // folds both. Anything other than `+`/`-` (`~`, `not`) declines.
    const ast::Expr* operand = &index;
    bool negated = false;
    while (const auto* unary = dynamic_cast<const ast::UnaryOp*>(operand)) {
        if (unary->op() == lexer::token_type::OP_MINUS) {
            negated = !negated;
        } else if (unary->op() != lexer::token_type::OP_PLUS) {
            return std::nullopt;
        }
        operand = &unary->operand();
    }

    const auto* constant = dynamic_cast<const ast::Constant*>(operand);
    if (constant == nullptr || constant->type() != lexer::token_type::LITERAL_INT) {
        return std::nullopt;
    }

    // Separators stripped first, then the base read explicitly. NOT strtoll
    // with base 0: C's auto-detection understands `0x` and LEADING-ZERO
    // octal, and Python's rules are neither -- its octal marker is `0o`, it
    // has `0b` as well, and a plain leading zero is not octal at all (`00` is
    // zero, `010` is a syntax error). strtoll also signals overflow by
    // CLAMPING to LLONG_MAX with errno set, which would hand back an
    // in-range-looking value for a magnitude nobody wrote; the accumulation
    // below rejects an overflowing magnitude outright, so there is no errno
    // to check and no clamped value can escape. Placement of the separators
    // is not validated -- the lexer already accepted the token.
    std::string digits;
    digits.reserve(constant->lexeme().size());
    for (const char character : constant->lexeme()) {
        if (character != '_') {
            digits.push_back(character);
        }
    }

    unsigned long long base = 10;
    std::size_t position = 0;
    if (digits.size() >= 2 && digits[0] == '0') {
        switch (digits[1]) {
        case 'x':
        case 'X':
            base = 16;
            position = 2;
            break;
        case 'o':
        case 'O':
            base = 8;
            position = 2;
            break;
        case 'b':
        case 'B':
            base = 2;
            position = 2;
            break;
        default:
            break;
        }
    }
    if (position >= digits.size()) {
        return std::nullopt;
    }

    const unsigned long long limit = negated ? TWO_TO_63 : TWO_TO_63 - 1;
    unsigned long long value = 0;
    for (; position < digits.size(); ++position) {
        const char character = digits[position];
        unsigned long long digit = 0;
        if (character >= '0' && character <= '9') {
            digit = static_cast<unsigned long long>(character - '0');
        } else if (character >= 'a' && character <= 'f') {
            digit = static_cast<unsigned long long>(character - 'a') + 10;
        } else if (character >= 'A' && character <= 'F') {
            digit = static_cast<unsigned long long>(character - 'A') + 10;
        } else {
            // Not a digit at all. Declining is right here, unlike in
            // literal_type.cpp's bound check, whose job is to avoid a false
            // OverflowError report: this function reports nothing, and a
            // lexeme it cannot read is a lexeme whose value it must not
            // claim to know.
            return std::nullopt;
        }
        if (digit >= base) {
            return std::nullopt;
        }
        // The exact overflow test, in the same shape literal_type.cpp uses:
        // value * base + digit > limit would itself overflow before the
        // comparison ran.
        if (value > limit / base || (value == limit / base && digit > limit % base)) {
            return std::nullopt;
        }
        value = value * base + digit;
    }

    if (!negated) {
        return static_cast<long long>(value);
    }
    // -2^63 cannot be formed by negating a long long, so it is named
    // directly rather than computed.
    if (value == TWO_TO_63) {
        return std::numeric_limits<long long>::min();
    }
    return -static_cast<long long>(value);
}

} // namespace

ExpressionTyper::ExpressionTyper(ScopeStack& scopes, const ClassTable& classes, TypeMap& types,
                                 NarrowingMap& narrowings, diagnostics::DiagnosticSink& sink)
    : scopes_(scopes), classes_(classes), types_(types), narrowings_(narrowings), sink_(sink) {}

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
    } else if (const auto* list_comp = dynamic_cast<const ast::ListComp*>(&expr)) {
        result = type_of_list_comp(*list_comp);
    } else {
        // Every other Expr kind is not yet implemented. Deliberately SILENT
        // -- not error() -- so an intermediate build never emits a
        // diagnostic a later task has to un-emit.
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
        return error(constant, DiagnosticKind::OverflowError,
                     "integer literal is too large for a 64-bit integer");
    }
    return literal_type(constant.type());
}

Type ExpressionTyper::type_of_name(const ast::Name& name) {
    const Resolution resolution = scopes_.resolve(name.identifier());
    if (resolution.binding == nullptr) {
        // Defect 2: a bare BUILTIN TYPE NAME used as a VALUE (`x: type = int`)
        // is mypy-clean (measured, mypy 1.18.1: `Success`). ScopeStack never
        // holds these names (nothing binds `int` itself), so a plain
        // resolution miss would otherwise fall straight into the NameError
        // below on some of the most ordinary code there is. mypy's own answer
        // here is unrepresentable in this model -- reveal_type(int) is the
        // OVERLOADED constructor, `Overload(def (str | Buffer | ... = ) ->
        // int, def (str | bytes | bytearray, base: SupportsIndex) -> int)` --
        // so this resolves to the metaclass itself, Class("type"), a seeded
        // builtin class (builtin_class_table.h), which is the closest
        // representable answer: `x: type = int` type-checks cleanly against
        // it, and calling the result through (`y = int` then `y(5)`) lands on
        // type_of_call_result's Class arm and reports NotImplementedError, a
        // missed error rather than a false one, preserving the hard invariant.
        //
        // Checked ONLY inside this `resolution.binding == nullptr` branch: a
        // live SCOPE BINDING of the same spelling (`def f(int: str) -> None:
        // print(int)`) makes resolve() come back non-null, so control never
        // enters this branch at all and falls through instead to the
        // ordinary binding-typed return below -- a parameter or local named
        // `int` still wins, exactly as it must.
        if (builtin_type_kind(name.identifier()).has_value()) {
            return Type::class_of("type");
        }
        // A USER class name used as a VALUE (`w = Widget`) is mypy-clean,
        // while we reported `name 'Widget' is not defined`. Class names are
        // deliberately never bound into ScopeStack -- both
        // type_of_attribute's class-object-receiver check and
        // type_of_name_call's bare-`C()` constructor branch depend on a bare
        // class name being ABSENT from it -- so a resolution miss is the
        // NORMAL state for one, not evidence it does not exist. ClassTable is
        // the sole source of truth, and is_class goes through
        // ClassTable::canonical_name, so a function-local class reached by
        // its scope-limited alias resolves here too.
        //
        // The CONSTRUCTOR is the measured-correct answer, not Class("type").
        // mypy 1.18.1 reveals a bare class name as its constructor signature:
        // `def () -> Widget` for a module-level class, `def () ->
        // Outer.Inner` for a nested one, `def () -> Local@6` for a
        // function-local one. Two sibling sites in this file already chose
        // exactly this, both saying the constructor is the closest
        // representable reading because the model has no `type[...]`:
        // type_of_attribute's nested-class branch
        // (`return classes_.constructor_type(qualified);`) and
        // type_of_name_call's bare-`C()` callee branch. This is that same
        // is_class/constructor_type pairing, passing the same BARE
        // identifier -- constructor_type canonicalises internally, so the
        // scope-limited alias of a function-local class is honoured here as
        // it is there. The payoff is that `w = Widget` then `w()` now types
        // as `Widget` and is clean, matching mypy, and that `w.x` no longer
        // draws a false `"type" has no attribute "x"` -- which Class("type")
        // did, because the seeded builtin class `type` carries bases but no
        // members.
        //
        // NOT unified with the builtin carve-out above, deliberately: `int`
        // is a model KIND, not a Class, so there is no ClassTable entry to
        // take a constructor from and Class("type") stays the answer there.
        // The asymmetry is the point, not an oversight.
        //
        // Two things this deliberately does NOT do. It does not fire when a
        // live scope binding of the same spelling exists (`def f(Widget: int)
        // -> int: return Widget.bit_length()`), because resolve() comes back
        // non-null there and control never enters this branch -- the check
        // must stay INSIDE it. And it makes `w = Widget` written ABOVE
        // `class Widget` silently clean, where mypy errors: classes are
        // declared by a whole-module pre-pass with no line information here
        // to order-check against. A missed error, accepted.
        if (classes_.is_class(name.identifier())) {
            return classes_.constructor_type(name.identifier());
        }
        // A bare builtin FUNCTION used as a VALUE. `f = len` is mypy-clean
        // (measured, mypy 1.18.1: `Success`, reveal_type `def (typing.Sized)
        // -> builtins.int`) and reported `name 'len' is not defined` -- the
        // smallest known violation of this project's hard invariant. The
        // reason it was invisible is structural: the class table holds
        // CLASSES and builtin_type_names holds model KINDS, and a builtin
        // function is neither, so both carve-outs above miss it.
        //
        // Unknown, NOT a Callable. This model has no signature for most of
        // these, Unknown is absorbing, and the result is therefore a MISSED
        // error rather than a false one: `f([1, 2])` types as Unknown and
        // reports nothing, and `y: type = len` -- which mypy rejects with
        // `expression has type "Callable[[Sized], int]"` -- becomes clean.
        // Measured, some of these are missed by BOTH oracles: `g = len; g(1,
        // 2, 3)` is a CPython `TypeError: len() takes exactly one argument (3
        // given)` as well as two mypy errors, and cythonpp reports nothing.
        // The asymmetry worth knowing: a CALL to an unmodelled builtin still
        // draws a loud NotImplementedError (type_of_name_call's builtin
        // deferral, just below in this file's sibling translation unit), but
        // a bare VALUE reference to that same builtin draws nothing here,
        // even though neither can actually be code-generated. Modelling real
        // signatures is separate work; builtin_call_table.h is where a
        // modelled one belongs.
        //
        // PLACED LAST of the three carve-outs, so a live scope binding
        // (checked by the enclosing branch), a model kind, and a user class of
        // the same spelling all still win -- `class hash: ...` must resolve to
        // the user's class, and `def f(len: str)` to the parameter.
        if (is_builtin_function_name(name.identifier())) {
            return Type::unknown();
        }
        return error(name, DiagnosticKind::NameError,
                     "name '" + name.identifier() + "' is not defined");
    }
    // THE ORDERING RULE (Task 11): a read is order-checked only against a
    // binding in its OWN immediately-enclosing scope; a read resolving
    // OUTWARD is never order-checked. >= , not >: `x = x + 1` where `x` is
    // not yet bound is a violation, because the right-hand side is evaluated
    // before the target is bound, so a read on the SAME line as its own
    // binding is already too late.
    //
    // order_exempt is checked
    // FIRST, ahead of the `>=`, because a parameter's declared_line is the
    // `def` line -- which for a one-line suite (`def f(x: int) -> None:
    // print(x)`) is the SAME line the body statement sits on, so `>=` alone
    // would misfire a false "used before definition" on every one-line def
    // that reads a parameter. A parameter can never genuinely be read before
    // its own definition (it is bound before the body runs, full stop), so
    // this is a real exemption, not a workaround: changing `>=` to `>`
    // instead would silently break AReadOnItsOwnBindingLineIsAViolation's
    // `x = x + 1` case, which relies on `>=` firing at module/local scope.
    if (resolution.in_own_scope && !resolution.binding->order_exempt &&
        resolution.binding->declared_line >= statement_line_) {
        return error(name, DiagnosticKind::NameError,
                     "name '" + name.identifier() + "' is used before definition");
    }
    // THE READ RULE: a narrowing entry for this path, if any, else the
    // binding's declared type. Checked AFTER the ordering rule above, so a
    // use-before-definition is still reported rather than answered with a
    // stale narrowing -- and after the resolution itself, so a name that
    // resolves OUTWARD (a closure reading an enclosing function's local) is
    // not narrowed by an entry belonging to a different scope. The
    // function-boundary reset is what makes that second point hold; see
    // NarrowingMap::clear.
    if (const std::optional<Type> narrowed = narrowings_.get(name.identifier())) {
        return *narrowed;
    }
    return resolution.binding->type;
}

void ExpressionTyper::set_statement_line(int line) { statement_line_ = line; }

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
                error(*elements[index], DiagnosticKind::TypeCheckerTypeError,
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
                error(*entries[index].key, DiagnosticKind::TypeCheckerTypeError,
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
    const std::optional<long long> literal = literal_integer_index(subscript.index());
    const RuleResult result = subscript_result(container, index, &classes_, literal);

    // WHICH MESSAGE, decided here because this is the only place with the
    // rendered types and the diagnostic wording -- subscript_result, like
    // every rule table in this project, reports nothing itself. A
    // NotApplicable from a tuple indexed by a LITERAL means the index was out
    // of range, not that its type was wrong (an `int` index is always the
    // right KIND for a tuple), and mypy's own message for it says exactly
    // that.
    //
    // The EMPTY tuple is included, not carved out: `t: tuple[()] = ()` then
    // `t[0]` draws `error: Tuple index out of range  [misc]` from mypy 1.18.1
    // and `IndexError: tuple index out of range` from CPython, so out-of-range
    // is the diagnosis for a zero-length tuple too -- every index is out of
    // range in one. Only a literal index gets it: measured, `e: tuple[()]`
    // subscripted by an `int` VARIABLE is mypy-clean (`reveal_type(e[j])` is
    // `Never`), and NotApplicable there is this compiler's own stricter call,
    // which the index-type wording still describes.
    std::string message = "invalid index type \"" + type_name(index) + "\" for \"" +
                          type_name(container) + "\"";
    if (container.kind == TypeKind::Tuple && literal.has_value()) {
        message = "tuple index out of range";
    }
    return apply(result, subscript, std::move(message));
}

Type ExpressionTyper::type_of_attribute(const ast::Attribute& attribute) {
    const Type declared = type_of_attribute_unnarrowed(attribute);
    if (const std::optional<NarrowedPath> path = narrowing_path_of(attribute)) {
        if (const std::optional<Type> narrowed = narrowings_.get(*path)) {
            return *narrowed;
        }
    }
    return declared;
}

Type ExpressionTyper::type_of_attribute_unnarrowed(const ast::Attribute& attribute) {
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
    // PRECEDENCE, checked here to fix a shadowing bug: a
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
    const std::string receiver_class = class_object_receiver(attribute.value());
    if (!receiver_class.empty()) {
        // A NESTED CLASS reached through its enclosing one: `Outer.Inner`,
        // `A.B.C`. ClassTable keys a nested class by its qualified name, so
        // this is a single is_class() question. Checked BEFORE
        // type_of_class_attribute, which searches members and methods only
        // and would report `"Outer" has no attribute "Inner"` -- a false
        // TypeError, since `x = Outer.Inner` is mypy-clean (mypy reveals
        // `type[Outer.Inner]`).
        //
        // The answer is the CONSTRUCTOR's type, matching what
        // type_of_name_call already chose for a bare `C` used as a callee:
        // this model has no `type[...]`, and the constructor is the closest
        // representable reading -- `Outer.Inner()` then constructs correctly
        // through the ordinary Call arm at any nesting depth. The cost is a
        // missed error, never a false one: `Outer.Inner.v` (a method reached
        // through two class objects) becomes an attribute access on a
        // Callable, which reports NotImplementedError rather than mypy's
        // real answer.
        const std::string qualified = receiver_class + "." + attribute.attribute();
        if (classes_.is_class(qualified)) {
            return classes_.constructor_type(qualified);
        }
        return type_of_class_attribute(Type::class_of(receiver_class), attribute,
                                       /*bind_self=*/false);
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
        return error(attribute, DiagnosticKind::NotImplementedError,
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
        return error(attribute, DiagnosticKind::NotImplementedError, kBuiltinMemberMessage);
    }
    // Unreachable: the switch is exhaustive over TypeKind and has no default,
    // so adding a kind warns here rather than silently falling through.
    return Type::unknown();
}

std::string ExpressionTyper::class_object_receiver(const ast::Expr& expr) {
    if (const auto* name = dynamic_cast<const ast::Name*>(&expr)) {
        // THE ROOT, and the one place the shadowing check belongs: a live
        // scope binding of this spelling means the expression is an ordinary
        // value, not a class object.
        if (!classes_.is_class(name->identifier()) ||
            scopes_.resolve(name->identifier()).binding != nullptr) {
            return {};
        }
        types_.insert(name, Type::class_of(name->identifier()));
        return name->identifier();
    }
    if (const auto* attribute = dynamic_cast<const ast::Attribute*>(&expr)) {
        const std::string prefix = class_object_receiver(attribute->value());
        if (prefix.empty()) {
            return {};
        }
        const std::string qualified = prefix + "." + attribute->attribute();
        if (!classes_.is_class(qualified)) {
            // The prefix is a class object but this segment is not a nested
            // class -- e.g. `Outer.count` or `Outer.method`. Not a
            // class-object chain, so the CALLER (type_of_attribute) resolves
            // it as an ordinary class attribute instead. The prefix's own
            // TypeMap entries are already recorded above and stay: they are
            // correct either way, and are exactly what the caller would
            // otherwise have to record for itself.
            return {};
        }
        types_.insert(attribute, Type::class_of(qualified));
        return qualified;
    }
    // Any other expression shape (a call, a subscript, a literal) is an
    // ordinary value: only a chain of plain names can spell a class object.
    return {};
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
        // defaulted_params is deliberately left alone: `self` never carries a
        // default, so dropping it changes the parameter count but not how
        // many of the trailing parameters are optional. That is exactly why
        // the count lives on the Type -- `c.greet()` on
        // `def greet(self, punct: str = "!")` stays clean through this
        // erase with nothing to remember to carry across.
        return bound;
    }
    // INSTANCE-MACHINERY MEMBERS: `__dict__` and `__module__`, present on any
    // instance of a DECLARED class (every class without `__slots__` gets an
    // instance `__dict__`, and every class gets a `__module__`) -- a
    // property of the class MACHINERY, not of `object`'s own member set,
    // which is why builtin_object_member_table.h's GENERATED kObjectMembers
    // deliberately excludes both (see that file's own "TWO NAMES A PRIOR
    // (WRONG) DRAFT OF THIS FIX INCLUDED" comment). Measured (mypy 1.18.1,
    // CPython): `class Plain: pass` / `p = Plain()` / `print(p.__dict__)` /
    // `print(p.__module__)` is `mypy --strict` Success and CPython prints
    // `{}` then `__main__`, and this compiler reported a false
    // `"Plain" has no attribute "__dict__"` before this check existed.
    // `bind_self` gates this to an INSTANCE receiver only (`Plain().__dict__`,
    // not the class-object `Plain.__dict__`, which this change does not
    // measure and does not touch). `is_object_itself` is the one guard that
    // keeps this from also clearing `object()` itself: CPython's `object()`
    // genuinely raises `AttributeError: 'object' object has no attribute
    // '__dict__'` (measured directly -- `object` carries neither name in
    // `dir(object)`), so a receiver that resolves to the SEEDED builtin `object`
    // row must keep falling through to the ordinary miss below, while every
    // OTHER declared class (including one that merely inherits from `object`,
    // explicitly or not) gets the clean answer. See
    // ClassTable::is_object_itself's own comment for why this is a flag
    // check, not a name comparison: a user `class object: pass` is an
    // ordinary declared class, not the builtin, and must get the clean answer
    // too.
    if (bind_self && !classes_.is_object_itself(receiver.name) &&
        (attribute.attribute() == "__dict__" || attribute.attribute() == "__module__")) {
        return Type::unknown();
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
    // builtin-inheriting classes only.
    if (classes_.inherits_builtin(receiver.name)) {
        return error(attribute, DiagnosticKind::NotImplementedError, kBuiltinMemberMessage);
    }
    // THE SECOND CARVE-OUT, inherits_builtin's sibling for the other half of
    // the seeded table: a class whose chain is, or reaches, one of the 97
    // SEEDED BUILTIN CLASS ROWS (Exception, OSError, slice, property,
    // memoryview, type, super, ...) has a name and bases recorded but no
    // MEMBERS at all -- the generated table has no typeshed, exactly like the
    // inherits_builtin carve-out just above, just for a Class-shaped row
    // instead of a modelled TypeKind. Measured (mypy 1.18.1, CPython): this
    // was 24 false TypeErrors on programs both oracles accept and run --
    // `super().__init__()` (mypy: Success; CPython prints normally) is the
    // most common one, since it is the idiomatic way to call a base
    // constructor from every subclass `__init__`. Also closes
    // `Exception("x").args`, `OSError().errno`, `class MyErr(ValueError):
    // pass` then `e.args`, `self.args` read inside a ValueError subclass
    // method, and others. The accepted cost is the identical MISSED-error
    // trade the carve-out above already makes: `super().nope()` and
    // `MyErr().nope` (both genuine mypy attr-defined errors) now report
    // NotImplementedError instead of TypeError. In REACHABLE code that is a
    // wash (exit 1 either way); inside UNREACHABLE code it is a real, if
    // sanctioned, tightening rather than a no-op -- TypeCheckerTypeError is
    // Suppressible and NotImplementedError is not (diagnostic_kind.h), so a
    // `MyErr().nope` sitting after a `return` used to be dropped silently
    // (exit 0) and is now reported (exit 1). Not a union-rule violation
    // (NotImplementedError is sanctioned regardless of reachability), and the
    // pre-existing inherits_builtin carve-out above already has the same
    // property, but say what actually happens rather than "no regression".
    // NOT the same predicate as inherits_builtin: that one is false for a
    // seeded CLASS base by design (see its own test,
    // InheritsBuiltinIsFalseForASeededExceptionBase) precisely because a
    // Class base is what this second carve-out exists to cover instead.
    //
    // kBuiltinMemberMessage's wording ("methods on builtin types are not
    // supported") was written for the carve-out above, where the missing
    // member usually IS a method (xs.append, s.upper). Reused here as-is
    // rather than given its own string: `MyErr().nope`, `super().__init__`
    // and `Exception("x").args` are read/called through a USER-DEFINED
    // class, and `.args` in particular is DATA, not a method -- neither noun
    // fits precisely. Left unremarked nowhere else in this file, so noted
    // here rather than silently reused: widening the wording is a genuine
    // option, just not one this change makes, since several corpus samples
    // (deferred_builtin_member_access.py and friends) already pin the exact
    // string for the carve-out above and would need updating in lockstep.
    if (classes_.inherits_builtin_class(receiver.name)) {
        return error(attribute, DiagnosticKind::NotImplementedError, kBuiltinMemberMessage);
    }
    // THE THIRD CARVE-OUT, and unlike the two above, not a chain walk at all:
    // `object`'s OWN real members (builtin_object_member_table.h's GENERATED
    // set, e.g. __class__/__repr__/__hash__/__str__) are not modelled
    // anywhere, and neither of the carve-outs above can reach them --
    // inherits_builtin_class's walk deliberately excludes `object` itself
    // (every class conceptually derives from it; see that function's own
    // comment), and a plain `class Plain: pass` records no bases at all, so
    // the walk never runs in the first place. Measured (mypy 1.18.1,
    // CPython): `class Plain: pass` / `p = Plain()` / `print(p.__class__)`
    // is `mypy --strict` Success and CPython prints
    // `<class '__main__.Plain'>`, and cythonpp reported a false
    // `"Plain" has no attribute "__class__"` before this check existed --
    // reachable identically on `object()` used directly, since ClassTable
    // seeds `object`'s own entry with no member-map entries either.
    // `is_object_member` is a fixed name-set membership test rather than
    // anything keyed on `receiver.name`, on purpose: `object`'s member set is
    // the same for every receiver, seeded or not, so there is nothing to
    // canonicalise or walk. Checked LAST, after every class-specific lookup
    // above has already missed, so a class that legitimately overrides one of
    // these names (`def __repr__(self) -> str: ...`) still resolves through
    // method_type first and never reaches this fallback at all.
    if (ClassTable::is_object_member(attribute.attribute())) {
        return Type::unknown();
    }
    // Every base is an ordinary user-class-shaped entry (neither a modelled
    // TypeKind nor a seeded builtin class row), so a miss here is a genuine
    // mypy attr-defined error.
    //
    // Routed through
    // strip_synthetic_class_prefix rather than quoting receiver.name raw --
    // an ISOLATED class (a losing top-level redefinition, or a function-local
    // class -- see type_checker.cpp's declare_isolated_class) has a
    // ClassTable KEY embedding TypeChecker's own internal "<tag>#<line>#"
    // disambiguator, which must never leak into a user-facing message: this
    // is, in practice, the single most reachable leak point of all (every
    // attribute miss on such a class goes through here), since it does not
    // even require the receiver to be `self`.
    return error(attribute, DiagnosticKind::TypeCheckerTypeError,
                 "\"" + strip_synthetic_class_prefix(receiver.name) + "\" has no attribute \"" +
                     attribute.attribute() + "\"");
}

// type_of_call, type_of_name_call, type_of_positional_call and
// type_of_call_result (the Call arm, Task 15) live in
// expression_typer_calls.cpp -- a second translation unit for this same
// class, split out once this file passed 700 lines. See that
// file's header comment for why this particular block was the seam.

Type ExpressionTyper::type_of_list_comp(const ast::ListComp& list_comp) {
    // A comprehension body is NOT a function boundary. Measured against mypy
    // 1.18.1: with `self.n` narrowed to int, `[self.n + 1 for _ in range(3)]`
    // reveals `builtins.list[builtins.int]` and is clean -- narrowing crosses
    // into the comprehension body. Clearing here would read `self.n` as its
    // declared `object` and make `object + 1` a false TypeError on a program
    // both oracles accept. So type_of_list_comp does NOT touch the narrowing
    // map; it pushes only its own Comprehension SCOPE, exactly as before.
    //
    // What it DOES do is hide the paths its own targets rebind, for its own
    // duration -- see NarrowingShadowGuard. Not being a boundary is exactly
    // what makes that necessary: a narrowing key carries no scope, so a
    // comprehension target shadowing an enclosing name would otherwise read
    // the enclosing name's narrowing for a variable that is not it. The
    // guard is constructed up front, before the first clause's iterable is
    // typed, since that iterable is evaluated in the ENCLOSING scope and
    // must still see every enclosing narrowing; nothing is hidden until a
    // target is actually bound below.
    NarrowingShadowGuard narrowing_shadow(narrowings_);

    // Constructed on the FIRST clause only, right before that clause's
    // target is bound -- not at the top of the function -- because the
    // first clause's own `iterable` is evaluated in the ENCLOSING scope
    // (Python semantics: `for x in xs` in `[y for x in xs]` sees `xs` from
    // outside), while every later clause's `iterable` is evaluated inside
    // the comprehension scope so it can read an earlier clause's target.
    // std::optional rather than a bare guard because "not pushed yet" is a
    // real state this function passes through, not merely a deferred
    // construction.
    std::optional<ComprehensionScopeGuard> guard;

    for (const ast::ComprehensionClause& clause : list_comp.clauses()) {
        const Type iterable = type_of(*clause.iterable, Type::unknown());
        const RuleResult element_result = element_type(iterable, &classes_);
        const Type element_type_value =
            apply(element_result, *clause.iterable, not_iterable_message(iterable));
        if (element_result.status != RuleResult::Status::Ok) {
            // apply() already reported. If this is the first clause, the
            // guard was never constructed and there is nothing to pop; if it
            // is a later clause, the guard's destructor pops on the way out
            // of this return -- exactly the case an RAII guard exists for.
            return Type::unknown();
        }

        if (!guard.has_value()) {
            guard.emplace(scopes_);
        }

        if (dynamic_cast<const ast::TupleExpr*>(clause.target.get()) != nullptr) {
            // mypy accepts a tuple target (`[k for k, v in pairs]` is
            // mypy-clean, revealing list[int]), so this must be
            // NotImplementedError, not TypeError. element_type of a
            // tuple[K, V] is the UNION K | V, not a positional pair, so
            // there is nothing correct to bind k/v to element-wise.
            return error(*clause.target, DiagnosticKind::NotImplementedError,
                         "tuple targets in comprehensions are not supported");
        }

        if (const auto* name_target = dynamic_cast<const ast::Name*>(clause.target.get())) {
            const ast::SourceSpan target_span = name_target->span();
            Binding binding;
            binding.type = element_type_value;
            binding.declared_line = target_span.start_line;
            // order_exempt=true: the THIRD site needing this exemption (see
            // Binding::order_exempt's own comment -- function parameters were
            // the first, a `for` target the second). A comprehension target
            // is bound here, before the element expression and every later
            // clause's iterable/condition are ever typed, so a same-line read
            // of it (`[v * v for v in values]`, all on one line) can never be
            // a genuine use-before-definition. Without this, the ordinary
            // `declared_line >= statement_line_` check misfires on nearly
            // every list comprehension, since a comprehension's target,
            // element and enclosing statement are overwhelmingly written on
            // one line.
            binding.order_exempt = true;
            scopes_.bind(name_target->identifier(), binding);
            // This target now means something entirely different from
            // whatever an enclosing scope's same-spelled name meant, so any
            // narrowing keyed on that spelling (and, by kill()'s prefix
            // rule, on anything beneath it) stops applying here. Restored
            // when this comprehension's own scope ends -- an enclosing
            // narrowing is shadowed, not destroyed. Done AFTER the bind
            // rather than before, purely so the two halves of "this name is
            // the comprehension's now" sit together; nothing between them
            // reads the map.
            narrowing_shadow.shadow(name_target->identifier());
        }
        // Every other assignable target shape (Attribute, Subscript) binds
        // no new name at all, so there is nothing to do for it here.

        for (const ast::ExprPtr& condition : clause.conditions) {
            type_of(*condition, Type::unknown());
        }
    }

    const Type element = type_of(list_comp.element(), Type::unknown());
    return Type::list_of(element);
}

Type ExpressionTyper::element_type_of(const ast::Expr& iterable_expr, const Type& iterable_type) {
    const RuleResult result = element_type(iterable_type, &classes_);
    return apply(result, iterable_expr, not_iterable_message(iterable_type));
}

std::string ExpressionTyper::not_iterable_message(const Type& iterable_type) {
    return "\"" + type_name(iterable_type) + "\" is not iterable";
}

Type ExpressionTyper::apply(const RuleResult& result, const ast::Expr& at,
                            std::string type_error_message) {
    switch (result.status) {
    case RuleResult::Status::Ok:
        return result.type;
    case RuleResult::Status::NotApplicable:
        return error(at, DiagnosticKind::TypeCheckerTypeError, std::move(type_error_message));
    case RuleResult::Status::Unsupported:
        return error(at, DiagnosticKind::NotImplementedError, unsupported_message(result.reason));
    }
    // Unreachable: the switch is exhaustive over RuleResult::Status and has no
    // default, so adding a status warns here rather than silently falling
    // through to a false Ok or a false TypeError.
    return Type::unknown();
}

Type ExpressionTyper::error(const ast::Expr& at, DiagnosticKind kind, std::string message) {
    const ast::SourceSpan span = at.span();
    sink_.report_error(diagnostic_code(kind), std::move(message), span.start_line,
                       span.start_column, suppressibility_of(kind));
    return Type::unknown();
}

} // namespace cythonpp::domain::semantic
