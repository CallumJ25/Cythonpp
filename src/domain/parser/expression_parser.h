#ifndef CYTHONPP_DOMAIN_PARSER_EXPRESSION_PARSER_H
#define CYTHONPP_DOMAIN_PARSER_EXPRESSION_PARSER_H

#include <string>

#include "domain/ast/expr.h"
#include "domain/ast/source_span.h"
#include "domain/diagnostics/diagnostic_sink.h"
#include "domain/lexer/token.h"
#include "domain/lexer/token_stream.h"

namespace cythonpp::domain::parser {

// Builds an ast::Expr from a post-IndentationPass token stream.
//
// Total: never throws. A malformed expression is reported to the sink and
// yields a null ExprPtr, matching how Lexer turns a bad character into a
// TOKEN_ERROR token and how IndentationPass reports and continues.
//
// Exactly one diagnostic per failed expression. AST nodes dereference their
// children and have no null state, so a half-built node cannot exist and
// there is nothing to attach a recovered subtree to. Recovery to the next
// NEWLINE belongs to the statement parser, which is the layer that knows
// where a statement ends.
//
// On failure the cursor is left *on* the offending token, so a caller knows
// where parsing stopped.
//
// Scope is exactly what the AST node set can represent. Every construct
// outside it -- ternaries, lambda, slices, f-strings, set displays, walrus --
// is a diagnostic naming the construct, not a parse.
class ExpressionParser {
public:
    ExpressionParser(lexer::TokenStream& tokens, diagnostics::DiagnosticSink& sink);

    // One expression. Stops at a comma; the caller decides what that means.
    ast::ExprPtr parse_expression();

    // Comma-joined expressions, building a TupleExpr if any comma was
    // present. A trailing comma is allowed. This is what a subscript index
    // and a bare `a, b` use.
    ast::ExprPtr parse_expression_list();

    // An assignment target: postfix-expressions only, optionally
    // comma-joined, validated to be assignable.
    //
    // Restricted rather than a full expression because `for x in y` parsed
    // with the full grammar swallows `x in y` as a Compare -- `in` is a
    // comparison operator. This grammar has no `in` in it, so it halts before
    // the keyword without a lookahead or a flag.
    ast::ExprPtr parse_target();

private:
    // Prefix '+', '-' and '~'. Recurses into itself so `- -x` nests.
    ast::ExprPtr parse_unary();

    // '**', right-associative. Its right operand goes through parse_unary(),
    // which is what makes `2 ** -1` legal, while its left operand is reached
    // only from parse_unary()'s fall-through, which is what makes `-2 ** 2`
    // group as `-(2 ** 2)`. An associativity flag in the precedence table
    // cannot express that asymmetry, which is why '**' is not in the table.
    ast::ExprPtr parse_power();

    ast::ExprPtr parse_atom();

    // Reports one SyntaxError and returns nullptr, so a failing rule reads as
    // a single `return error(...)`.
    ast::ExprPtr error(const lexer::Token& token, std::string message);
    ast::ExprPtr error_at(ast::SourceSpan span, std::string message);

    lexer::TokenStream&          tokens_;
    diagnostics::DiagnosticSink& sink_;
};

} // namespace cythonpp::domain::parser

#endif // CYTHONPP_DOMAIN_PARSER_EXPRESSION_PARSER_H
