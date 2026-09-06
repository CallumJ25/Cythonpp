#include "expression_parser.h"

#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "domain/ast/bin_op.h"
#include "domain/ast/compare.h"
#include "domain/ast/constant.h"
#include "domain/ast/name.h"
#include "domain/ast/unary_op.h"
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

} // namespace

ExpressionParser::ExpressionParser(lexer::TokenStream& tokens, diagnostics::DiagnosticSink& sink)
    : tokens_(tokens), sink_(sink) {}

ast::ExprPtr ExpressionParser::parse_expression() { return parse_not_test(); }

// Filled in at Task 9.
ast::ExprPtr ExpressionParser::parse_expression_list() { return parse_expression(); }

// Filled in at Task 11.
ast::ExprPtr ExpressionParser::parse_target() { return parse_atom(); }

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
    // Becomes parse_postfix() at Task 10, which inserts trailers between the
    // atom and the exponent.
    ast::ExprPtr base = parse_atom();
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

ast::ExprPtr ExpressionParser::error(const lexer::Token& token, std::string message) {
    return error_at(ast::span_of(token), std::move(message));
}

ast::ExprPtr ExpressionParser::error_at(ast::SourceSpan span, std::string message) {
    sink_.report_error("SyntaxError", std::move(message), span.start_line, span.start_column);
    return nullptr;
}

} // namespace cythonpp::domain::parser
