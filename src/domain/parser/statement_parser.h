#ifndef CYTHONPP_DOMAIN_PARSER_STATEMENT_PARSER_H
#define CYTHONPP_DOMAIN_PARSER_STATEMENT_PARSER_H

#include <memory>
#include <string>
#include <vector>

#include "domain/ast/module.h"
#include "domain/ast/parameter.h"
#include "domain/ast/stmt.h"
#include "domain/diagnostics/diagnostic_sink.h"
#include "domain/lexer/token.h"
#include "domain/lexer/token_stream.h"
#include "expression_parser.h"

namespace cythonpp::domain::parser {

// Builds an ast::Module from a post-IndentationPass token stream.
//
// Total: never throws. A malformed statement is reported to the sink and
// dropped from its body, matching how Lexer turns a bad character into a
// TOKEN_ERROR token and how IndentationPass reports and continues.
//
// Exactly one diagnostic per failed statement. A sub-parse returning null (or
// an empty suite) has already reported; the caller propagates the failure
// silently rather than adding a second message.
//
// Failed statements are dropped rather than replaced by an error node. The
// argument that rejected ErrorExpr holds here and is stronger: dropping a
// statement leaves a smaller but complete tree, because a body is a vector
// and a shorter vector is still a valid body, whereas dropping an expression
// would leave a hole in a node whose child pointer cannot be null.
//
// This parser owns the read cursor and hands the same TokenStream to a member
// ExpressionParser. TokenStream::advance() mutates, so a stream has one
// reader at a time: here the statement parser drives and the expression
// parser is a subroutine that borrows the cursor and returns it.
//
// Block structure comes from the INDENT/DEDENT tokens IndentationPass
// produces, whose balance -- exactly as many DEDENTs as INDENTs, for any
// input -- this parser leans on directly and does not re-verify.
class StatementParser {
public:
    StatementParser(lexer::TokenStream& tokens, diagnostics::DiagnosticSink& sink);

    // Never null. A file whose every statement failed yields an empty Module,
    // not a null one; the errors are read from the sink.
    std::unique_ptr<ast::Module> parse_module();

private:
    // The shared statement loop, used by parse_module and by parse_suite's
    // block form. Stops at DEDENT or TOKEN_EOF without consuming either, so
    // the caller decides what the boundary meant.
    //
    // Carries a position-based progress guard: if an iteration ends where it
    // began, the loop advances one token unconditionally. Resynchronisation
    // deliberately stops before INDENT/DEDENT and the expression parser
    // leaves its cursor *on* the offending token, so several paths can
    // legitimately consume nothing -- the guard makes termination a property
    // rather than an argument about each of them.
    std::vector<ast::StmtPtr> parse_statement_list();

    // One statement, simple or compound. Null on failure, already reported.
    ast::StmtPtr parse_statement();

    // The simple statements on one logical line: `a; b; c` with an optional
    // trailing `;`, ending at NEWLINE. Python's grammar makes semicolons and
    // one-line suites the same production, so this serves both.
    //
    // Returns false if any statement on the line failed. The ones that
    // parsed before the failure are still appended to `into`.
    bool parse_simple_statement_line(std::vector<ast::StmtPtr>& into);

    // One simple statement -- no suite of its own. Null on failure.
    ast::StmtPtr parse_simple_statement();

    // `return`, `return expr`, `return a, b`.
    ast::StmtPtr parse_return();

    // The three statements told apart only by what follows their leading
    // expression: AnnAssign, Assign and ExprStmt.
    ast::StmtPtr parse_expression_statement();

    // `target = value`. `target` is the already-parsed leading expression.
    ast::StmtPtr parse_assignment(ast::ExprPtr target);

    // `target: annotation` with an optional `= value`.
    ast::StmtPtr parse_annotated_assignment(ast::ExprPtr target);

    // ':' and everything after it: either NEWLINE INDENT statements DEDENT,
    // or a simple-statement line on the same line. Consumes the ':' itself,
    // so the "expected ':'" diagnostic exists in one place rather than five.
    //
    // Empty on failure. A Python suite can never legitimately be empty, so
    // empty is unambiguous.
    std::vector<ast::StmtPtr> parse_suite();

    // The optional `else:` suite If, While and For all carry. Empty when
    // there is no `else`, which is indistinguishable from a failed one --
    // the failure was already reported, and both cases yield no statements.
    std::vector<ast::StmtPtr> parse_else_clause();

    ast::StmtPtr parse_if();
    ast::StmtPtr parse_while();
    ast::StmtPtr parse_for();
    ast::StmtPtr parse_function_def();
    ast::StmtPtr parse_class_def();

    // The parameter list between '(' and ')'. Returns false on failure, with
    // the parameters parsed before it still appended to `into`.
    bool parse_parameters(std::vector<ast::Parameter>& into);

    // A construct outside the supported subset: reports `message` against
    // `keyword` and consumes the rest of its logical line, so the caller gets
    // one diagnostic and a clean boundary rather than a cascade from the
    // tokens the construct would have owned.
    ast::StmtPtr reject(const lexer::Token& keyword, std::string message);

    // An INDENT at statement position, which IndentationPass emits without
    // reporting. Consumes the INDENT, skips the block tracking nesting depth,
    // and consumes the matching DEDENT. Reports nothing itself: the caller
    // reports, so the message can name the position it was reached from.
    void skip_unexpected_block();

    // Panic-mode resynchronisation. Consumes through the next NEWLINE, and
    // stops *before* INDENT, DEDENT and TOKEN_EOF without consuming them.
    //
    // Never consuming INDENT/DEDENT is load-bearing, not tidiness:
    // parse_suite terminates by seeing its DEDENT, so eating one would make
    // the enclosing suite swallow the rest of the file into the wrong block.
    void synchronize();

    // Consumes the NEWLINE (or SEMICOLON, or TOKEN_EOF) that must end a
    // simple statement. Reports and returns false if something else is there.
    bool expect_end_of_statement();

    // Reports one SyntaxError and returns nullptr, so a failing rule reads as
    // a single `return error(...)`. The pair mirrors ExpressionParser's:
    // error() takes the token, error_at() takes a span, for the cases where
    // the position to blame is a whole subtree or a bracket's opener rather
    // than the token the cursor happens to be on.
    ast::StmtPtr error(const lexer::Token& token, std::string message);
    ast::StmtPtr error_at(ast::SourceSpan span, std::string message);

    lexer::TokenStream&          tokens_;
    diagnostics::DiagnosticSink& sink_;
    ExpressionParser             expressions_;
};

} // namespace cythonpp::domain::parser

#endif // CYTHONPP_DOMAIN_PARSER_STATEMENT_PARSER_H
