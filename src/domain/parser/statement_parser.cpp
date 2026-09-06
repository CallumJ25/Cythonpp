#include "statement_parser.h"

#include <cstddef>
#include <utility>

#include <string>

#include "assignability.h"
#include "domain/ast/ann_assign.h"
#include "domain/ast/assign.h"
#include "domain/ast/break.h"
#include "domain/ast/continue.h"
#include "domain/ast/expr_stmt.h"
#include "domain/ast/for.h"
#include "domain/ast/function_def.h"
#include "domain/ast/if.h"
#include "domain/ast/pass.h"
#include "domain/ast/return.h"
#include "domain/ast/source_span.h"
#include "domain/ast/tuple_expr.h"
#include "domain/ast/while.h"
#include "domain/lexer/token_category.h"
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
bool is_compound_keyword(token_type type) {
    return type == token_type::KEYWORD_IF || type == token_type::KEYWORD_WHILE ||
           type == token_type::KEYWORD_FOR || type == token_type::KEYWORD_DEF;
}

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

        if (tokens_.check(token_type::INDENT)) {
            // IndentationPass emits this without reporting, so the diagnostic
            // is ours. Report once, discard the block, and carry on -- the
            // statements around it are still worth parsing.
            const ast::SourceSpan span = ast::span_of(tokens_.peek());
            sink_.report_error("IndentationError", "unexpected indent", span.start_line,
                               span.start_column);
            skip_unexpected_block();
            continue;
        }

        // Dispatch happens here rather than inside parse_statement, because a
        // simple-statement *line* can yield several statements -- `pass;
        // break` is two -- and a function returning one StmtPtr cannot say
        // so. Compound statements own a suite and are always exactly one, so
        // they go through parse_statement. is_compound_keyword names that
        // set; while/for/def/class join it in later tasks.
        if (is_compound_keyword(tokens_.peek().type())) {
            ast::StmtPtr statement = parse_statement();
            if (statement != nullptr) {
                body.push_back(std::move(statement));
            } else {
                // A compound statement can fail after already reaching a
                // boundary; synchronizing again would eat the next statement.
                if (!at_statement_boundary()) {
                    synchronize();
                }
                if (tokens_.check(token_type::INDENT)) {
                    // Silently. This block is orphaned as a consequence of the
                    // error already reported, not independently, and "exactly
                    // one diagnostic per failed statement" covers the whole
                    // statement -- header and body together.
                    skip_unexpected_block();
                }
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

ast::StmtPtr StatementParser::parse_statement() {
    switch (tokens_.peek().type()) {
        case token_type::KEYWORD_IF:    return parse_if();
        case token_type::KEYWORD_WHILE: return parse_while();
        case token_type::KEYWORD_FOR:   return parse_for();
        case token_type::KEYWORD_DEF:   return parse_function_def();
        default:                        break;
    }
    // Only reachable if is_compound_keyword and this switch disagree.
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

void StatementParser::skip_unexpected_block() {
    if (!tokens_.match(token_type::INDENT)) {
        return;
    }
    // Nesting is tracked so a block containing its own deeper block is
    // skipped whole. IndentationPass guarantees balance, so the matching
    // DEDENT exists and this loop cannot run to EOF on real lexer output --
    // the at_end() check is for a hand-built stream.
    int depth = 1;
    while (depth > 0 && !tokens_.at_end()) {
        if (tokens_.check(token_type::INDENT)) {
            ++depth;
        } else if (tokens_.check(token_type::DEDENT)) {
            --depth;
        }
        tokens_.advance();
    }
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

bool StatementParser::at_statement_boundary() const {
    std::size_t index = tokens_.position();
    if (index == 0) {
        return true;
    }
    // Step back past comments, which survive IndentationPass and which the
    // cursor itself skips -- so the token physically before the cursor is not
    // necessarily the one that logically precedes it.
    do {
        --index;
    } while (index > 0 && tokens_.at(index).type() == token_type::COMMENT_SINGLE);

    const token_type previous = tokens_.at(index).type();
    if (previous == token_type::COMMENT_SINGLE) {
        // Every token before the cursor was trivia -- a boundary exactly like
        // index == 0 above, since there is no real statement back there to
        // have left the cursor mid-line.
        return true;
    }
    return previous == token_type::NEWLINE || previous == token_type::INDENT ||
           previous == token_type::DEDENT;
}

bool StatementParser::expect_end_of_statement() {
    if (ends_a_statement(tokens_.peek().type())) {
        return true;
    }
    error(tokens_.peek(), "expected a newline after the statement");
    return false;
}

std::vector<ast::StmtPtr> StatementParser::parse_suite() {
    if (!tokens_.match(token_type::COLON)) {
        error(tokens_.peek(), "expected ':'");
        return {};
    }

    // The one-line form: `if x: return 1`. Python's grammar makes this and a
    // semicolon-separated line the same production, so it is the same call.
    if (!tokens_.match(token_type::NEWLINE)) {
        // A scratch vector spliced only on success, matching what Task 3's
        // parse_statement_list does at module level. parse_simple_statement_line
        // appends every statement it parsed before hitting trouble, but a
        // malformed line is dropped whole rather than half-kept: the leading
        // `pass` in `if x: pass pass` is part of the same bad line, not a
        // separate salvageable statement. Returning empty here makes the
        // caller drop the whole compound statement, which is how a failed
        // block suite already behaves.
        std::vector<ast::StmtPtr> line;
        if (!parse_simple_statement_line(line)) {
            return {};
        }
        return line;
    }

    if (!tokens_.check(token_type::INDENT)) {
        error(tokens_.peek(), "expected an indented block");
        return {};
    }
    tokens_.advance();

    std::vector<ast::StmtPtr> body = parse_statement_list();
    // IndentationPass guarantees a matching DEDENT, so match() rather than a
    // check-and-report: there is no reachable branch where it is absent.
    tokens_.match(token_type::DEDENT);
    return body;
}

ast::StmtPtr StatementParser::parse_if() {
    const ast::SourceSpan keyword_span = ast::span_of(tokens_.peek());
    tokens_.advance(); // 'if' or 'elif' -- both reach here, which is the point

    // parse_expression, not parse_expression_list: `if a, b:` is not valid
    // Python, and letting a comma through would build a TupleExpr condition
    // that is always truthy.
    ast::ExprPtr condition = expressions_.parse_expression();
    if (condition == nullptr) {
        return nullptr; // already reported
    }

    std::vector<ast::StmtPtr> body = parse_suite();
    if (body.empty()) {
        return nullptr; // already reported
    }

    std::vector<ast::StmtPtr> orelse;
    if (tokens_.check(token_type::KEYWORD_ELIF)) {
        // elif is not a node: it nests as an If inside this one's orelse,
        // which is how Python's own grammar defines it. See if.h.
        ast::StmtPtr nested = parse_if();
        if (nested == nullptr) {
            return nullptr;
        }
        orelse.push_back(std::move(nested));
    } else if (!parse_else_clause(orelse)) {
        return nullptr; // already reported
    }

    // Bound before the moves below: argument evaluation order is unspecified.
    const ast::SourceSpan end = orelse.empty() ? body.back()->span() : orelse.back()->span();
    const ast::SourceSpan span = ast::merge(keyword_span, end);
    return std::make_unique<ast::If>(span, std::move(condition), std::move(body),
                                     std::move(orelse));
}

bool StatementParser::parse_else_clause(std::vector<ast::StmtPtr>& into) {
    if (!tokens_.check(token_type::KEYWORD_ELSE)) {
        return true; // no else is not a failure
    }
    tokens_.advance();

    std::vector<ast::StmtPtr> body = parse_suite();
    if (body.empty()) {
        return false; // already reported
    }
    into = std::move(body);
    return true;
}

ast::StmtPtr StatementParser::parse_while() {
    const ast::SourceSpan keyword_span = ast::span_of(tokens_.peek());
    tokens_.advance();

    ast::ExprPtr condition = expressions_.parse_expression();
    if (condition == nullptr) {
        return nullptr;
    }

    std::vector<ast::StmtPtr> body = parse_suite();
    if (body.empty()) {
        return nullptr;
    }
    std::vector<ast::StmtPtr> orelse;
    if (!parse_else_clause(orelse)) {
        return nullptr; // already reported
    }

    const ast::SourceSpan end = orelse.empty() ? body.back()->span() : orelse.back()->span();
    const ast::SourceSpan span = ast::merge(keyword_span, end);
    return std::make_unique<ast::While>(span, std::move(condition), std::move(body),
                                        std::move(orelse));
}

ast::StmtPtr StatementParser::parse_for() {
    const ast::SourceSpan keyword_span = ast::span_of(tokens_.peek());
    tokens_.advance();

    // parse_target, not parse_expression: `for x in y` parsed with the full
    // grammar swallows `x in y` as a Compare, because `in` is a comparison
    // operator. parse_target's grammar has no `in` in it, so it halts before
    // the keyword without a lookahead or a flag.
    ast::ExprPtr target = expressions_.parse_target();
    if (target == nullptr) {
        return nullptr;
    }
    if (!tokens_.match(token_type::OP_IN)) {
        return error(tokens_.peek(), "expected 'in' after the for target");
    }

    ast::ExprPtr iterable = expressions_.parse_expression_list();
    if (iterable == nullptr) {
        return nullptr;
    }

    std::vector<ast::StmtPtr> body = parse_suite();
    if (body.empty()) {
        return nullptr;
    }
    std::vector<ast::StmtPtr> orelse;
    if (!parse_else_clause(orelse)) {
        return nullptr; // already reported
    }

    const ast::SourceSpan end = orelse.empty() ? body.back()->span() : orelse.back()->span();
    const ast::SourceSpan span = ast::merge(keyword_span, end);
    return std::make_unique<ast::For>(span, std::move(target), std::move(iterable),
                                      std::move(body), std::move(orelse));
}

ast::StmtPtr StatementParser::parse_function_def() {
    const ast::SourceSpan keyword_span = ast::span_of(tokens_.peek());
    tokens_.advance(); // 'def'

    const lexer::Token& name_token = tokens_.peek();
    if (!lexer::has_category(name_token.type(), lexer::token_category::IDENTIFIER)) {
        return error(name_token, "expected a function name");
    }
    std::string name = name_token.lexeme();
    tokens_.advance();

    std::vector<ast::Parameter> params;
    if (!parse_parameters(params)) {
        return nullptr; // already reported
    }

    ast::ExprPtr return_annotation;
    if (tokens_.match(token_type::OP_ARROW)) {
        return_annotation = expressions_.parse_expression();
        if (return_annotation == nullptr) {
            return nullptr;
        }
    }

    std::vector<ast::StmtPtr> body = parse_suite();
    if (body.empty()) {
        return nullptr;
    }

    // Bound before the moves below: argument evaluation order is unspecified.
    const ast::SourceSpan span = ast::merge(keyword_span, body.back()->span());
    return std::make_unique<ast::FunctionDef>(span, std::move(name), std::move(params),
                                              std::move(return_annotation), std::move(body));
}

bool StatementParser::parse_parameters(std::vector<ast::Parameter>& into) {
    const lexer::Token& opener = tokens_.peek();
    // The opener is held in a local rather than on a delimiter stack: the C++
    // call stack already is one, and an explicit copy would need a push/pop
    // matched across every early return. Same convention as ExpressionParser.
    const ast::SourceSpan opener_span = ast::span_of(opener);
    if (!tokens_.match(token_type::OPEN_PAREN)) {
        error(opener, "expected '(' after the function name");
        return false;
    }

    if (tokens_.match(token_type::CLOSE_PAREN)) {
        return true;
    }

    while (true) {
        const lexer::Token& name_token = tokens_.peek();
        if (!lexer::has_category(name_token.type(), lexer::token_category::IDENTIFIER)) {
            error(name_token, "expected a parameter name");
            return false;
        }
        const ast::SourceSpan name_span = ast::span_of(name_token);
        std::string name = name_token.lexeme();
        tokens_.advance();

        ast::ExprPtr annotation;
        if (tokens_.match(token_type::COLON)) {
            annotation = expressions_.parse_expression();
            if (annotation == nullptr) {
                return false;
            }
        }

        ast::ExprPtr default_value;
        if (tokens_.match(token_type::OP_ASSIGN)) {
            default_value = expressions_.parse_expression();
            if (default_value == nullptr) {
                return false;
            }
        }

        // Widest end available, read before either pointer moves.
        ast::SourceSpan end = name_span;
        if (annotation != nullptr) {
            end = annotation->span();
        }
        if (default_value != nullptr) {
            end = default_value->span();
        }
        const ast::SourceSpan span = ast::merge(name_span, end);
        into.emplace_back(span, std::move(name), std::move(annotation), std::move(default_value));

        if (tokens_.match(token_type::COMMA)) {
            if (tokens_.check(token_type::CLOSE_PAREN)) {
                break; // trailing comma
            }
            continue;
        }
        break;
    }

    if (!tokens_.match(token_type::CLOSE_PAREN)) {
        // Reported against the opener, not the current token: the lexer emits
        // a NEWLINE at EOF even inside an unclosed bracket, so wherever the
        // cursor has reached says nothing useful about where the mistake is.
        error_at(opener_span, "expected ')' to close the parameter list");
        return false;
    }
    return true;
}

ast::StmtPtr StatementParser::error(const lexer::Token& token, std::string message) {
    return error_at(ast::span_of(token), std::move(message));
}

ast::StmtPtr StatementParser::error_at(ast::SourceSpan span, std::string message) {
    sink_.report_error("SyntaxError", std::move(message), span.start_line, span.start_column);
    return nullptr;
}

} // namespace cythonpp::domain::parser
