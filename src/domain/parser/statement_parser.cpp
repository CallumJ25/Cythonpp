#include "statement_parser.h"

#include <cstddef>
#include <utility>

#include "assignability.h"
#include "domain/ast/ann_assign.h"
#include "domain/ast/assign.h"
#include "domain/ast/break.h"
#include "domain/ast/continue.h"
#include "domain/ast/expr_stmt.h"
#include "domain/ast/pass.h"
#include "domain/ast/return.h"
#include "domain/ast/source_span.h"
#include "domain/ast/tuple_expr.h"
#include "domain/lexer/token_type.h"

namespace cythonpp::domain::parser {

namespace {

using lexer::token_type;

// The tokens that end a simple statement. TOKEN_EOF is here because a file
// whose last line has no trailing newline still ends a statement.
bool ends_a_statement(token_type type) {
    return type == token_type::NEWLINE || type == token_type::SEMICOLON ||
           type == token_type::TOKEN_EOF;
}

// The statements that own a suite. Everything else is a simple statement and
// can share a logical line with a semicolon.
//
// Empty until Task 7 adds def/class/if/while/for, which is why every
// statement currently takes the simple path.
bool is_compound_keyword(token_type) { return false; }

// The thirteen augmented-assignment operators. Each is a diagnostic rather
// than a parse: there is no AugAssign node, and desugaring `x += 1` to
// `x = x + 1` would record a lie, since Python's += evaluates the target once
// and is in-place for mutable types.
bool is_augmented_assign(token_type type) {
    switch (type) {
        case token_type::OP_PLUS_ASSIGN:
        case token_type::OP_MINUS_ASSIGN:
        case token_type::OP_STAR_ASSIGN:
        case token_type::OP_SLASH_ASSIGN:
        case token_type::OP_DOUBLE_SLASH_ASSIGN:
        case token_type::OP_PERCENT_ASSIGN:
        case token_type::OP_DOUBLE_STAR_ASSIGN:
        case token_type::OP_AT_ASSIGN:
        case token_type::OP_AMPERSAND_ASSIGN:
        case token_type::OP_PIPE_ASSIGN:
        case token_type::OP_CARET_ASSIGN:
        case token_type::OP_RIGHT_SHIFT_ASSIGN:
        case token_type::OP_LEFT_SHIFT_ASSIGN:
            return true;
        default:
            return false;
    }
}

} // namespace

StatementParser::StatementParser(lexer::TokenStream& tokens, diagnostics::DiagnosticSink& sink)
    : tokens_(tokens), sink_(sink), expressions_(tokens, sink) {}

std::unique_ptr<ast::Module> StatementParser::parse_module() {
    // TokenStream::peek() throws std::out_of_range on an empty stream. Real
    // Lexer output always ends in TOKEN_EOF, so this only bites a hand-built
    // stream -- but the "never throws" contract must hold for any TokenStream.
    if (tokens_.empty()) {
        return std::make_unique<ast::Module>(ast::SourceSpan{1, 1, 1, 1},
                                             std::vector<ast::StmtPtr>());
    }

    const ast::SourceSpan start = ast::span_of(tokens_.peek());
    std::vector<ast::StmtPtr> body;

    while (!tokens_.at_end()) {
        std::vector<ast::StmtPtr> more = parse_statement_list();
        for (ast::StmtPtr& statement : more) {
            body.push_back(std::move(statement));
        }

        // parse_statement_list only ever stops at DEDENT or TOKEN_EOF (that is
        // the negation of its own while condition). If it stopped at
        // TOKEN_EOF, advance() is documented as a no-op there, so this is
        // harmless and the outer while's own !at_end() check ends the loop
        // next iteration. If it stopped at a stray module-level DEDENT --
        // unreachable through real Lexer + IndentationPass output, which is
        // balanced: every DEDENT is consumed by the suite that opened the
        // matching INDENT, but not re-verified here -- this consumes it
        // rather than reporting it, which is also what guarantees this
        // loop's own progress on such a stream instead of spinning.
        tokens_.advance();
    }

    // The whole file, first token to last, so trailing blank lines are inside
    // the module's span rather than dangling past its end.
    const ast::SourceSpan span = ast::merge(start, ast::span_of(tokens_.peek()));
    return std::make_unique<ast::Module>(span, std::move(body));
}

std::vector<ast::StmtPtr> StatementParser::parse_statement_list() {
    std::vector<ast::StmtPtr> body;
    while (!tokens_.at_end() && !tokens_.check(token_type::DEDENT)) {
        const std::size_t before = tokens_.position();

        if (tokens_.check(token_type::NEWLINE)) {
            // A stray NEWLINE at statement position: an empty logical line
            // the lexer did emit, or the one left by a recovered statement.
            tokens_.advance();
            continue;
        }

        // Dispatch happens here rather than inside parse_statement, because a
        // simple-statement *line* can yield several statements -- `pass;
        // break` is two -- and a function returning one StmtPtr cannot say
        // so. Compound statements own a suite and are always exactly one, so
        // they go through parse_statement. Task 7 fills in the compound set;
        // until then is_compound_keyword is never true.
        if (is_compound_keyword(tokens_.peek().type())) {
            ast::StmtPtr statement = parse_statement();
            if (statement != nullptr) {
                body.push_back(std::move(statement));
            } else {
                synchronize();
            }
        } else {
            // A scratch vector, not `body` directly: parse_simple_statement_line
            // still appends every statement it parses before hitting trouble
            // (that is its documented contract), but a malformed line is
            // dropped as a whole here, not just its offending tail. `pass
            // pass` is one bad line, and the leading `pass` that parsed fine
            // before the second one turned up is not a separate, salvageable
            // statement -- it is part of the same line panic-mode recovery
            // throws away. Only a successful line's statements are merged
            // into the real body.
            std::vector<ast::StmtPtr> line;
            if (parse_simple_statement_line(line)) {
                for (ast::StmtPtr& statement : line) {
                    body.push_back(std::move(statement));
                }
            } else {
                synchronize();
            }
        }

        // The progress guard. Several paths can legitimately consume nothing:
        // synchronize() stops before INDENT/DEDENT, and ExpressionParser
        // leaves its cursor on the offending token. Rather than argue each
        // one terminates, make it true by construction.
        if (tokens_.position() == before) {
            tokens_.advance();
        }
    }
    return body;
}

// Compound statements (def, class, if, while, for) are added in Task 7, which
// is what gives this function anything to dispatch. Until then the compound
// set is empty and every statement goes down the simple path in
// parse_statement_list, so this is never called.
ast::StmtPtr StatementParser::parse_statement() {
    return error(tokens_.peek(), "expected a statement");
}

bool StatementParser::parse_simple_statement_line(std::vector<ast::StmtPtr>& into) {
    while (true) {
        ast::StmtPtr statement = parse_simple_statement();
        if (statement == nullptr) {
            return false;
        }
        into.push_back(std::move(statement));

        if (tokens_.match(token_type::SEMICOLON)) {
            if (ends_a_statement(tokens_.peek().type())) {
                break; // trailing semicolon
            }
            continue;
        }
        break;
    }

    if (tokens_.check(token_type::TOKEN_EOF)) {
        return true;
    }
    if (tokens_.match(token_type::NEWLINE)) {
        return true;
    }
    error(tokens_.peek(), "expected a newline after the statement");
    return false;
}

ast::StmtPtr StatementParser::parse_simple_statement() {
    const lexer::Token& first = tokens_.peek();
    switch (first.type()) {
        case token_type::KEYWORD_PASS:
            tokens_.advance();
            return std::make_unique<ast::Pass>(ast::span_of(first));
        case token_type::KEYWORD_BREAK:
            tokens_.advance();
            return std::make_unique<ast::Break>(ast::span_of(first));
        case token_type::KEYWORD_CONTINUE:
            tokens_.advance();
            return std::make_unique<ast::Continue>(ast::span_of(first));
        case token_type::KEYWORD_RETURN:
            return parse_return();
        default:
            return parse_expression_statement();
    }
}

ast::StmtPtr StatementParser::parse_return() {
    const lexer::Token& keyword = tokens_.peek();
    const ast::SourceSpan keyword_span = ast::span_of(keyword);
    tokens_.advance();

    if (ends_a_statement(tokens_.peek().type())) {
        return std::make_unique<ast::Return>(keyword_span, nullptr);
    }

    // parse_expression_list rather than parse_expression: `return a, b`
    // returns one tuple, which is what the TupleExpr node is for.
    ast::ExprPtr value = expressions_.parse_expression_list();
    if (value == nullptr) {
        return nullptr; // already reported
    }
    // Bound to a local before the move: argument evaluation order is
    // unspecified, so merging inside the make_unique call can read a
    // moved-from pointer.
    const ast::SourceSpan span = ast::merge(keyword_span, value->span());
    return std::make_unique<ast::Return>(span, std::move(value));
}

ast::StmtPtr StatementParser::parse_expression_statement() {
    ast::ExprPtr first = expressions_.parse_expression_list();
    if (first == nullptr) {
        return nullptr; // already reported
    }

    if (tokens_.check(token_type::COLON)) {
        return parse_annotated_assignment(std::move(first));
    }
    if (tokens_.check(token_type::OP_ASSIGN)) {
        return parse_assignment(std::move(first));
    }
    if (is_augmented_assign(tokens_.peek().type())) {
        return error(tokens_.peek(), "augmented assignment is not supported");
    }

    const ast::SourceSpan span = first->span();
    return std::make_unique<ast::ExprStmt>(span, std::move(first));
}

ast::StmtPtr StatementParser::parse_assignment(ast::ExprPtr target) {
    if (!is_assignable(*target)) {
        return error_at(target->span(), not_assignable_message(*target));
    }
    tokens_.advance(); // '='

    ast::ExprPtr value = expressions_.parse_expression_list();
    if (value == nullptr) {
        return nullptr;
    }
    const ast::SourceSpan span = ast::merge(target->span(), value->span());
    return std::make_unique<ast::Assign>(span, std::move(target), std::move(value));
}

ast::StmtPtr StatementParser::parse_annotated_assignment(ast::ExprPtr target) {
    if (!is_assignable(*target)) {
        return error_at(target->span(), not_assignable_message(*target));
    }
    // AnnAssign holds one target, and Python does not allow annotating a
    // tuple, so this is rejected here rather than deferred to the checker.
    if (dynamic_cast<const ast::TupleExpr*>(target.get()) != nullptr) {
        return error_at(target->span(), "only single targets can be annotated");
    }
    tokens_.advance(); // ':'

    // parse_expression, not parse_expression_list: `x: int, str` is not a
    // thing. A composite annotation is spelled `x: tuple[int, str]`, whose
    // comma is inside the subscript.
    ast::ExprPtr annotation = expressions_.parse_expression();
    if (annotation == nullptr) {
        return nullptr;
    }

    ast::ExprPtr value;
    if (tokens_.match(token_type::OP_ASSIGN)) {
        value = expressions_.parse_expression_list();
        if (value == nullptr) {
            return nullptr;
        }
    }

    const ast::SourceSpan end = value != nullptr ? value->span() : annotation->span();
    const ast::SourceSpan span = ast::merge(target->span(), end);
    return std::make_unique<ast::AnnAssign>(span, std::move(target), std::move(annotation),
                                            std::move(value));
}

void StatementParser::synchronize() {
    while (!tokens_.at_end()) {
        if (tokens_.check(token_type::NEWLINE)) {
            tokens_.advance();
            return;
        }
        if (tokens_.check(token_type::INDENT) || tokens_.check(token_type::DEDENT)) {
            return; // never consumed -- see the header
        }
        tokens_.advance();
    }
}

bool StatementParser::expect_end_of_statement() {
    if (ends_a_statement(tokens_.peek().type())) {
        return true;
    }
    error(tokens_.peek(), "expected a newline after the statement");
    return false;
}

ast::StmtPtr StatementParser::error(const lexer::Token& token, std::string message) {
    return error_at(ast::span_of(token), std::move(message));
}

ast::StmtPtr StatementParser::error_at(ast::SourceSpan span, std::string message) {
    sink_.report_error("SyntaxError", std::move(message), span.start_line, span.start_column);
    return nullptr;
}

} // namespace cythonpp::domain::parser
