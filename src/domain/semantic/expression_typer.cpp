#include "expression_typer.h"

#include <string>
#include <utility>
#include <vector>

#include "domain/ast/source_span.h"
#include "domain/lexer/keyword_table.h"
#include "domain/lexer/operator_table.h"
#include "domain/lexer/token_type.h"
#include "literal_type.h"
#include "operator_rules.h"
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
    } else {
        // Every other Expr kind is a later task's arm (container displays:
        // 13, Subscript/Attribute: 14, Call: 15, ListComp: 16, ...).
        // Deliberately SILENT -- not error() -- so an intermediate build
        // never emits a diagnostic a later task has to un-emit.
        result = Type::unknown();
    }
    // Every expression the typer types gets an entry, unconditionally --
    // including this silent fallback, since type_of() was still asked to
    // type it. Annotation subtrees never reach here at all: AnnotationResolver
    // walks those directly and type_of() is never called on one.
    types_.insert(&expr, result);
    (void)expected; // Consumed starting with Task 13's container displays.
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
