#include "expression_parser.h"

#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "domain/ast/attribute.h"
#include "domain/ast/bin_op.h"
#include "domain/ast/bool_op.h"
#include "domain/ast/call.h"
#include "domain/ast/compare.h"
#include "domain/ast/comprehension_clause.h"
#include "domain/ast/constant.h"
#include "domain/ast/dict_expr.h"
#include "domain/ast/list_comp.h"
#include "domain/ast/list_expr.h"
#include "domain/ast/name.h"
#include "domain/ast/subscript.h"
#include "domain/ast/tuple_expr.h"
#include "domain/ast/unary_op.h"
#include "domain/lexer/operator_table.h"
#include "domain/lexer/token_category.h"
#include "domain/lexer/token_type.h"
#include "precedence_table.h"

namespace cythonpp::domain::parser {

namespace {

using lexer::token_type;

bool is_string_literal(token_type type) {
    return type == token_type::LITERAL_STRING || type == token_type::LITERAL_BYTES;
}

// The single-token comparison operators. OP_IS and OP_NOT are absent: both
// can begin a two-token operator, so parse_comparison handles them first.
bool is_simple_comparison(token_type type) {
    switch (type) {
        case token_type::OP_LESS:
        case token_type::OP_GREATER:
        case token_type::OP_EQUAL:
        case token_type::OP_NOT_EQUAL:
        case token_type::OP_LESS_EQUAL:
        case token_type::OP_GREATER_EQUAL:
        case token_type::OP_IN:
            return true;
        default:
            return false;
    }
}

bool is_closing_delimiter(token_type type) {
    return type == token_type::CLOSE_PAREN || type == token_type::CLOSE_BRACKET ||
           type == token_type::CLOSE_BRACE;
}

// True where a trailing comma is legal -- that is, where no further element
// can begin.
bool ends_a_sequence(token_type type) {
    return is_closing_delimiter(type) || type == token_type::NEWLINE ||
           type == token_type::TOKEN_EOF || type == token_type::INDENT ||
           type == token_type::DEDENT;
}

bool is_assignable(const ast::Expr& expr) {
    if (dynamic_cast<const ast::Name*>(&expr) != nullptr) {
        return true;
    }
    if (dynamic_cast<const ast::Attribute*>(&expr) != nullptr) {
        return true;
    }
    if (dynamic_cast<const ast::Subscript*>(&expr) != nullptr) {
        return true;
    }
    if (const auto* tuple = dynamic_cast<const ast::TupleExpr*>(&expr)) {
        if (tuple->elements().empty()) {
            return false;
        }
        for (const ast::ExprPtr& element : tuple->elements()) {
            if (!is_assignable(*element)) {
                return false;
            }
        }
        return true;
    }
    return false;
}

// CPython's phrasing family, so the message reads like the one a user has
// seen before.
std::string not_assignable_message(const ast::Expr& expr) {
    if (dynamic_cast<const ast::Constant*>(&expr) != nullptr) {
        return "cannot assign to literal";
    }
    if (dynamic_cast<const ast::Call*>(&expr) != nullptr) {
        return "cannot assign to function call";
    }
    return "cannot assign to this expression";
}

} // namespace

ExpressionParser::ExpressionParser(lexer::TokenStream& tokens, diagnostics::DiagnosticSink& sink)
    : tokens_(tokens), sink_(sink) {}

ast::ExprPtr ExpressionParser::parse_expression() {
    ast::ExprPtr value = parse_or_test();
    if (value == nullptr) {
        return nullptr;
    }
    // `x if c else y` needs an IfExp node the AST deliberately does not have.
    // Checked here rather than deeper down so a comprehension's own `if`,
    // which is parsed by parse_or_test, is unaffected.
    if (tokens_.check(token_type::KEYWORD_IF)) {
        return error(tokens_.peek(), "conditional expressions are not supported");
    }
    // Same shape, same reason. parse_atom rejects a ':=' that *starts* an
    // operand, but in `n := 1` the target fills the operand slot first, so
    // that branch never sees it -- and the enclosing bracket rule would
    // otherwise report "never closed" and point at the wrong thing.
    if (tokens_.check(token_type::OP_WALRUS)) {
        return error(tokens_.peek(), "assignment expressions are not supported");
    }
    return value;
}

ast::ExprPtr ExpressionParser::parse_expression_list() {
    ast::ExprPtr first = parse_expression();
    if (first == nullptr) {
        return nullptr;
    }
    if (!tokens_.check(token_type::COMMA)) {
        return first;
    }
    std::vector<ast::ExprPtr> elements;
    elements.push_back(std::move(first));
    while (tokens_.match(token_type::COMMA)) {
        if (ends_a_sequence(tokens_.peek().type())) {
            break; // trailing comma
        }
        ast::ExprPtr next = parse_expression();
        if (next == nullptr) {
            return nullptr;
        }
        elements.push_back(std::move(next));
    }
    const ast::SourceSpan span = ast::merge(elements.front()->span(), elements.back()->span());
    return std::make_unique<ast::TupleExpr>(span, std::move(elements));
}

ast::ExprPtr ExpressionParser::parse_target() {
    ast::ExprPtr first = parse_postfix();
    if (first == nullptr) {
        return nullptr;
    }
    if (!is_assignable(*first)) {
        return error_at(first->span(), not_assignable_message(*first));
    }
    if (!tokens_.check(token_type::COMMA)) {
        return first;
    }

    std::vector<ast::ExprPtr> elements;
    elements.push_back(std::move(first));
    while (tokens_.match(token_type::COMMA)) {
        if (tokens_.check(token_type::OP_IN) || ends_a_sequence(tokens_.peek().type())) {
            break; // trailing comma
        }
        ast::ExprPtr next = parse_postfix();
        if (next == nullptr) {
            return nullptr;
        }
        if (!is_assignable(*next)) {
            return error_at(next->span(), not_assignable_message(*next));
        }
        elements.push_back(std::move(next));
    }
    const ast::SourceSpan span = ast::merge(elements.front()->span(), elements.back()->span());
    return std::make_unique<ast::TupleExpr>(span, std::move(elements));
}

ast::ExprPtr ExpressionParser::parse_or_test() {
    ast::ExprPtr first = parse_and_test();
    if (first == nullptr) {
        return nullptr;
    }
    if (!tokens_.check(token_type::OP_OR)) {
        return first;
    }
    std::vector<ast::ExprPtr> values;
    values.push_back(std::move(first));
    while (tokens_.match(token_type::OP_OR)) {
        ast::ExprPtr next = parse_and_test();
        if (next == nullptr) {
            return nullptr;
        }
        values.push_back(std::move(next));
    }
    const ast::SourceSpan span = ast::merge(values.front()->span(), values.back()->span());
    return std::make_unique<ast::BoolOp>(span, token_type::OP_OR, std::move(values));
}

ast::ExprPtr ExpressionParser::parse_and_test() {
    ast::ExprPtr first = parse_not_test();
    if (first == nullptr) {
        return nullptr;
    }
    if (!tokens_.check(token_type::OP_AND)) {
        return first;
    }
    std::vector<ast::ExprPtr> values;
    values.push_back(std::move(first));
    while (tokens_.match(token_type::OP_AND)) {
        ast::ExprPtr next = parse_not_test();
        if (next == nullptr) {
            return nullptr;
        }
        values.push_back(std::move(next));
    }
    const ast::SourceSpan span = ast::merge(values.front()->span(), values.back()->span());
    return std::make_unique<ast::BoolOp>(span, token_type::OP_AND, std::move(values));
}

ast::ExprPtr ExpressionParser::parse_not_test() {
    // Prefix position only. In `a not in b` the `not` arrives after an
    // operand, inside parse_comparison's loop, so the two never collide.
    if (tokens_.check(token_type::OP_NOT)) {
        const lexer::Token& op = tokens_.advance();
        ast::ExprPtr operand = parse_not_test();
        if (operand == nullptr) {
            return nullptr;
        }
        const ast::SourceSpan span = ast::merge(ast::span_of(op), operand->span());
        return std::make_unique<ast::UnaryOp>(span, token_type::OP_NOT, std::move(operand));
    }
    return parse_comparison();
}

ast::ExprPtr ExpressionParser::parse_comparison() {
    ast::ExprPtr left = parse_binary(LOWEST_BINARY_LEVEL);
    if (left == nullptr) {
        return nullptr;
    }

    std::vector<ast::Compare::Rest> rest;
    while (true) {
        const token_type next = tokens_.peek().type();
        token_type op = token_type::TOKEN_ERROR;

        if (next == token_type::OP_NOT) {
            // The scanner has no OP_NOT_IN, so the pair is folded here into
            // the single token_type Compare::Rest holds.
            if (tokens_.peek(1).type() != token_type::OP_IN) {
                return error(tokens_.peek(1), "expected 'in' after 'not'");
            }
            tokens_.advance();
            tokens_.advance();
            op = token_type::OP_NOT_IN;
        } else if (next == token_type::OP_IS) {
            tokens_.advance();
            op = tokens_.match(token_type::OP_NOT) ? token_type::OP_IS_NOT : token_type::OP_IS;
        } else if (is_simple_comparison(next)) {
            tokens_.advance();
            op = next;
        } else {
            break;
        }

        ast::ExprPtr right = parse_binary(LOWEST_BINARY_LEVEL);
        if (right == nullptr) {
            return nullptr;
        }
        rest.push_back(ast::Compare::Rest{op, std::move(right)});
    }

    if (rest.empty()) {
        return left;
    }
    const ast::SourceSpan span = ast::merge(left->span(), rest.back().operand->span());
    return std::make_unique<ast::Compare>(span, std::move(left), std::move(rest));
}

ast::ExprPtr ExpressionParser::parse_binary(int min_level) {
    ast::ExprPtr left = parse_unary();
    if (left == nullptr) {
        return nullptr;
    }
    while (true) {
        const std::optional<BinaryPrecedence> precedence =
            binary_precedence_of(tokens_.peek().type());
        if (!precedence.has_value() || precedence->level < min_level) {
            return left;
        }
        const lexer::Token& op = tokens_.advance();

        // Every table entry is left-associative, so the right operand is
        // parsed one level tighter. That is what groups `a - b - c` as
        // `(a - b) - c` rather than `a - (b - c)`.
        ast::ExprPtr right = parse_binary(precedence->level + 1);
        if (right == nullptr) {
            return nullptr;
        }
        const ast::SourceSpan span = ast::merge(left->span(), right->span());
        left = std::make_unique<ast::BinOp>(span, op.type(), std::move(left), std::move(right));
    }
}

ast::ExprPtr ExpressionParser::parse_unary() {
    const token_type type = tokens_.peek().type();
    if (type == token_type::OP_PLUS || type == token_type::OP_MINUS ||
        type == token_type::OP_TILDE) {
        const lexer::Token& op = tokens_.advance();
        ast::ExprPtr operand = parse_unary();
        if (operand == nullptr) {
            return nullptr;
        }
        const ast::SourceSpan span = ast::merge(ast::span_of(op), operand->span());
        return std::make_unique<ast::UnaryOp>(span, type, std::move(operand));
    }
    return parse_power();
}

ast::ExprPtr ExpressionParser::parse_power() {
    ast::ExprPtr base = parse_postfix();
    if (base == nullptr) {
        return nullptr;
    }
    if (!tokens_.match(token_type::OP_DOUBLE_STAR)) {
        return base;
    }
    ast::ExprPtr exponent = parse_unary();
    if (exponent == nullptr) {
        return nullptr;
    }
    const ast::SourceSpan span = ast::merge(base->span(), exponent->span());
    return std::make_unique<ast::BinOp>(span, token_type::OP_DOUBLE_STAR, std::move(base),
                                        std::move(exponent));
}

ast::ExprPtr ExpressionParser::parse_postfix() {
    ast::ExprPtr value = parse_atom();
    if (value == nullptr) {
        return nullptr;
    }
    while (true) {
        if (tokens_.check(token_type::DOT)) {
            value = parse_attribute(std::move(value));
        } else if (tokens_.check(token_type::OPEN_BRACKET)) {
            value = parse_subscript(std::move(value));
        } else if (tokens_.check(token_type::OPEN_PAREN)) {
            value = parse_call(std::move(value));
        } else {
            return value;
        }
        if (value == nullptr) {
            return nullptr;
        }
    }
}

ast::ExprPtr ExpressionParser::parse_attribute(ast::ExprPtr value) {
    tokens_.advance(); // '.'
    if (!lexer::has_category(tokens_.peek().type(), lexer::token_category::IDENTIFIER)) {
        return error(tokens_.peek(), "expected an attribute name after '.'");
    }
    const lexer::Token& name = tokens_.advance();
    const ast::SourceSpan span = ast::merge(value->span(), ast::span_of(name));
    return std::make_unique<ast::Attribute>(span, std::move(value), name.lexeme());
}

ast::ExprPtr ExpressionParser::parse_subscript(ast::ExprPtr value) {
    const lexer::Token& opener = tokens_.advance(); // '['

    // Subscript holds one index Expr and there is no Slice node, so a colon
    // here or after the index is the construct, not a typo.
    if (tokens_.check(token_type::COLON)) {
        return error(tokens_.peek(), "slices are not supported");
    }
    ast::ExprPtr index = parse_expression_list();
    if (index == nullptr) {
        return nullptr;
    }
    if (tokens_.check(token_type::COLON)) {
        return error(tokens_.peek(), "slices are not supported");
    }
    if (!tokens_.check(token_type::CLOSE_BRACKET)) {
        return unclosed(opener);
    }
    const lexer::Token& closer = tokens_.advance();
    const ast::SourceSpan span = ast::merge(value->span(), ast::span_of(closer));
    return std::make_unique<ast::Subscript>(span, std::move(value), std::move(index));
}

ast::ExprPtr ExpressionParser::parse_call(ast::ExprPtr callee) {
    const lexer::Token& opener = tokens_.advance(); // '('

    std::vector<ast::ExprPtr> args;
    if (!tokens_.check(token_type::CLOSE_PAREN)) {
        while (true) {
            ast::ExprPtr arg = parse_expression();
            if (arg == nullptr) {
                return nullptr;
            }
            // Call holds positional arguments only. Both of these are caught
            // after the argument parses, because `k` and `x` are ordinary
            // expressions until the token that follows them says otherwise.
            if (tokens_.check(token_type::OP_ASSIGN)) {
                return error(tokens_.peek(), "keyword arguments are not supported");
            }
            if (tokens_.check(token_type::KEYWORD_FOR)) {
                return error(tokens_.peek(), "generator expressions are not supported");
            }
            args.push_back(std::move(arg));
            if (!tokens_.match(token_type::COMMA)) {
                break;
            }
            if (tokens_.check(token_type::CLOSE_PAREN)) {
                break; // trailing comma
            }
        }
    }

    if (!tokens_.check(token_type::CLOSE_PAREN)) {
        return unclosed(opener);
    }
    const lexer::Token& closer = tokens_.advance();
    const ast::SourceSpan span = ast::merge(callee->span(), ast::span_of(closer));
    return std::make_unique<ast::Call>(span, std::move(callee), std::move(args));
}

ast::ExprPtr ExpressionParser::parse_atom() {
    const lexer::Token& token = tokens_.peek();

    // Rejections first. The f-string cases in particular MUST precede the
    // category tests below: FSTRING_* carry OBJECT, so they would otherwise
    // fall into the Constant rule and build a node out of a fragment.
    switch (token.type()) {
        case token_type::FSTRING_START:
        case token_type::FSTRING_MIDDLE:
        case token_type::FSTRING_END:
            return error(token, "f-strings are not supported");
        case token_type::KEYWORD_LAMBDA:
            return error(token, "lambda expressions are not supported");
        case token_type::KEYWORD_AWAIT:
            return error(token, "await expressions are not supported");
        case token_type::KEYWORD_YIELD:
            return error(token, "yield expressions are not supported");
        case token_type::OP_STAR:
        case token_type::OP_DOUBLE_STAR:
            return error(token, "starred expressions are not supported");
        case token_type::OP_WALRUS:
            return error(token, "assignment expressions are not supported");
        default:
            break;
    }

    if (tokens_.check(token_type::OPEN_PAREN)) {
        return parse_paren_atom();
    }

    if (tokens_.check(token_type::OPEN_BRACKET)) {
        return parse_bracket_atom();
    }

    if (tokens_.check(token_type::OPEN_BRACE)) {
        return parse_brace_atom();
    }

    // One rule for IDENTIFIER and all thirteen TYPE_* spellings. ScanContext
    // makes `int` a TYPE_INT in `list[int]` and an IDENTIFIER in
    // `print(int)`; Name stores the lexeme either way, and erasing the
    // distinction is correct because it is the same name.
    if (lexer::has_category(token.type(), lexer::token_category::IDENTIFIER)) {
        const lexer::Token& name = tokens_.advance();
        return std::make_unique<ast::Name>(ast::span_of(name), name.lexeme());
    }

    if (lexer::has_category(token.type(), lexer::token_category::OBJECT)) {
        const lexer::Token& literal = tokens_.advance();
        if (is_string_literal(literal.type()) && is_string_literal(tokens_.peek().type())) {
            return error(tokens_.peek(), "implicit string concatenation is not supported");
        }
        return std::make_unique<ast::Constant>(ast::span_of(literal), literal.type(),
                                               literal.lexeme());
    }

    return error(token, "expected an expression");
}

ast::ExprPtr ExpressionParser::parse_paren_atom() {
    const lexer::Token& opener = tokens_.advance();

    if (tokens_.check(token_type::CLOSE_PAREN)) {
        const lexer::Token& closer = tokens_.advance();
        const ast::SourceSpan span = ast::merge(ast::span_of(opener), ast::span_of(closer));
        return std::make_unique<ast::TupleExpr>(span, std::vector<ast::ExprPtr>{});
    }

    std::vector<ast::ExprPtr> elements;
    bool saw_comma = false;
    while (true) {
        // Nothing that can start an expression remains: this is the unclosed
        // bracket itself, not a missing operand, so report it against the
        // opener rather than letting parse_expression's own "expected an
        // expression" mask it. The indentation pass synthesizes a NEWLINE at
        // end-of-file even inside an unclosed bracket (confirmed by
        // inspecting the token stream for "("), so TOKEN_EOF alone is not a
        // sufficient check -- ends_a_sequence catches that NEWLINE too. See
        // the task report for why the brief's literal code needed this
        // addition.
        if (ends_a_sequence(tokens_.peek().type())) {
            return unclosed(opener);
        }
        ast::ExprPtr element = parse_expression();
        if (element == nullptr) {
            return nullptr;
        }
        elements.push_back(std::move(element));
        if (!tokens_.match(token_type::COMMA)) {
            break;
        }
        saw_comma = true;
        if (tokens_.check(token_type::CLOSE_PAREN)) {
            break; // trailing comma
        }
    }

    if (!tokens_.check(token_type::CLOSE_PAREN)) {
        return unclosed(opener);
    }
    const lexer::Token& closer = tokens_.advance();

    if (!saw_comma) {
        // Grouping. The inner node is returned unchanged, so `(a)` keeps a's
        // span: nodes are immutable, and widening it would mean rebuilding
        // the subtree for a distinction nothing downstream uses.
        return std::move(elements.front());
    }
    const ast::SourceSpan span = ast::merge(ast::span_of(opener), ast::span_of(closer));
    return std::make_unique<ast::TupleExpr>(span, std::move(elements));
}

ast::ExprPtr ExpressionParser::parse_bracket_atom() {
    const lexer::Token& opener = tokens_.advance(); // '['

    if (tokens_.check(token_type::CLOSE_BRACKET)) {
        const lexer::Token& closer = tokens_.advance();
        const ast::SourceSpan span = ast::merge(ast::span_of(opener), ast::span_of(closer));
        return std::make_unique<ast::ListExpr>(span, std::vector<ast::ExprPtr>{});
    }

    // Nothing that can start an expression remains, so this is the unclosed
    // bracket itself rather than a missing operand. The lexer closes an
    // unterminated final line with a NEWLINE even inside an open bracket
    // (lexer.cpp), so checking for TOKEN_EOF alone would miss it and
    // parse_expression's "expected an expression" would mask the real error.
    if (ends_a_sequence(tokens_.peek().type())) {
        return unclosed(opener);
    }
    ast::ExprPtr first = parse_expression();
    if (first == nullptr) {
        return nullptr;
    }
    if (tokens_.check(token_type::KEYWORD_FOR)) {
        return parse_list_comp(opener, std::move(first));
    }

    std::vector<ast::ExprPtr> elements;
    elements.push_back(std::move(first));
    while (tokens_.match(token_type::COMMA)) {
        // ends_a_sequence rather than check(CLOSE_BRACKET), for the same
        // reason: it covers both a legal trailing comma and `[1,` running out
        // of input. Either way the closer check below decides which it was.
        if (ends_a_sequence(tokens_.peek().type())) {
            break;
        }
        ast::ExprPtr next = parse_expression();
        if (next == nullptr) {
            return nullptr;
        }
        elements.push_back(std::move(next));
    }

    if (!tokens_.check(token_type::CLOSE_BRACKET)) {
        return unclosed(opener);
    }
    const lexer::Token& closer = tokens_.advance();
    const ast::SourceSpan span = ast::merge(ast::span_of(opener), ast::span_of(closer));
    return std::make_unique<ast::ListExpr>(span, std::move(elements));
}

ast::ExprPtr ExpressionParser::parse_list_comp(const lexer::Token& opener, ast::ExprPtr element) {
    std::vector<ast::ComprehensionClause> clauses;
    while (tokens_.match(token_type::KEYWORD_FOR)) {
        ast::ComprehensionClause clause;

        clause.target = parse_target();
        if (clause.target == nullptr) {
            return nullptr;
        }
        if (!tokens_.match(token_type::OP_IN)) {
            return error(tokens_.peek(), "expected 'in' after a comprehension target");
        }

        // or_test rather than parse_expression, in both positions: Python's
        // grammar uses or_test here, and going through parse_expression would
        // misread this clause's own `if` as the start of a rejected ternary.
        clause.iterable = parse_or_test();
        if (clause.iterable == nullptr) {
            return nullptr;
        }
        while (tokens_.match(token_type::KEYWORD_IF)) {
            ast::ExprPtr condition = parse_or_test();
            if (condition == nullptr) {
                return nullptr;
            }
            clause.conditions.push_back(std::move(condition));
        }
        clauses.push_back(std::move(clause));
    }

    if (!tokens_.check(token_type::CLOSE_BRACKET)) {
        return unclosed(opener);
    }
    const lexer::Token& closer = tokens_.advance();
    const ast::SourceSpan span = ast::merge(ast::span_of(opener), ast::span_of(closer));
    return std::make_unique<ast::ListComp>(span, std::move(element), std::move(clauses));
}

ast::ExprPtr ExpressionParser::parse_brace_atom() {
    const lexer::Token& opener = tokens_.advance(); // '{'

    // `{}` is the empty dict in Python, not the empty set.
    if (tokens_.check(token_type::CLOSE_BRACE)) {
        const lexer::Token& closer = tokens_.advance();
        const ast::SourceSpan span = ast::merge(ast::span_of(opener), ast::span_of(closer));
        return std::make_unique<ast::DictExpr>(span, std::vector<ast::DictExpr::Entry>{});
    }

    // As in parse_bracket_atom: nothing can start an expression here, so the
    // brace is unclosed rather than an operand missing. The lexer emits a
    // NEWLINE at end of file even inside an open bracket, so TOKEN_EOF alone
    // is not a sufficient test.
    if (ends_a_sequence(tokens_.peek().type())) {
        return unclosed(opener);
    }
    ast::ExprPtr first_key = parse_expression();
    if (first_key == nullptr) {
        return nullptr;
    }

    // What follows the first expression is what tells the three brace
    // constructs apart, which is why the check happens here and not earlier.
    if (!tokens_.check(token_type::COLON)) {
        if (tokens_.check(token_type::KEYWORD_FOR)) {
            return error(tokens_.peek(), "set comprehensions are not supported");
        }
        return error(tokens_.peek(), "set displays are not supported");
    }
    tokens_.advance(); // ':'

    ast::ExprPtr first_value = parse_expression();
    if (first_value == nullptr) {
        return nullptr;
    }
    if (tokens_.check(token_type::KEYWORD_FOR)) {
        return error(tokens_.peek(), "dict comprehensions are not supported");
    }

    std::vector<ast::DictExpr::Entry> entries;
    entries.push_back(ast::DictExpr::Entry{std::move(first_key), std::move(first_value)});

    while (tokens_.match(token_type::COMMA)) {
        if (tokens_.check(token_type::CLOSE_BRACE)) {
            break; // trailing comma
        }
        ast::ExprPtr key = parse_expression();
        if (key == nullptr) {
            return nullptr;
        }
        if (!tokens_.match(token_type::COLON)) {
            return error(tokens_.peek(), "expected ':' in a dict display");
        }
        ast::ExprPtr value = parse_expression();
        if (value == nullptr) {
            return nullptr;
        }
        entries.push_back(ast::DictExpr::Entry{std::move(key), std::move(value)});
    }

    if (!tokens_.check(token_type::CLOSE_BRACE)) {
        return unclosed(opener);
    }
    const lexer::Token& closer = tokens_.advance();
    const ast::SourceSpan span = ast::merge(ast::span_of(opener), ast::span_of(closer));
    return std::make_unique<ast::DictExpr>(span, std::move(entries));
}

ast::ExprPtr ExpressionParser::unclosed(const lexer::Token& opener) {
    const lexer::Token& found = tokens_.peek();
    const std::string opener_lexeme(lexer::operator_lexeme_of(opener.type()));

    // A closer of the wrong kind is a different mistake from running out of
    // input, and naming both brackets is what makes it fixable.
    if (is_closing_delimiter(found.type())) {
        return error(found, "closing '" + std::string(lexer::operator_lexeme_of(found.type())) +
                                "' does not match '" + opener_lexeme + "' opened on line " +
                                std::to_string(opener.line_number()));
    }
    return error(opener, "'" + opener_lexeme + "' was never closed");
}

ast::ExprPtr ExpressionParser::error(const lexer::Token& token, std::string message) {
    return error_at(ast::span_of(token), std::move(message));
}

ast::ExprPtr ExpressionParser::error_at(ast::SourceSpan span, std::string message) {
    sink_.report_error("SyntaxError", std::move(message), span.start_line, span.start_column);
    return nullptr;
}

} // namespace cythonpp::domain::parser
