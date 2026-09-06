#ifndef CYTHONPP_TESTS_DOMAIN_PARSER_PARSE_TEST_HELPERS_H
#define CYTHONPP_TESTS_DOMAIN_PARSER_PARSE_TEST_HELPERS_H

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <vector>

#include "domain/ast/ast_printer.h"
#include "domain/ast/expr.h"
#include "domain/ast/source_span.h"
#include "domain/diagnostics/diagnostic.h"
#include "domain/diagnostics/diagnostic_sink.h"
#include "domain/lexer/indentation_pass.h"
#include "domain/lexer/lexer.h"
#include "domain/lexer/token_stream.h"
#include "domain/parser/expression_parser.h"

namespace cythonpp::domain::parser {
namespace test_support {

// Everything the parser tests assert on. Owns the expression, so a test can
// dynamic_cast the root when the printed form is not enough -- AstPrinter
// erases Constant::type(), rendering LITERAL_INT "1" and LITERAL_FLOAT "1"
// identically, so literal tests must read the type directly.
struct ParseResult {
    ast::ExprPtr expression;
    std::vector<diagnostics::Diagnostic> diagnostics;

    bool succeeded() const { return expression != nullptr; }

    std::string printed() const {
        return expression == nullptr ? std::string() : ast::AstPrinter().print(*expression);
    }

    ast::SourceSpan span() const {
        return expression == nullptr ? ast::SourceSpan{0, 0, 0, 0} : expression->span();
    }
};

// Which entry point to drive. parse() defaults to Expression.
enum class Entry { Expression, ExpressionList, Target };

// One parse through the real chain: source text -> Lexer -> IndentationPass
// -> ExpressionParser. Nothing is hand-constructed, because a hand-built
// token vector would prove nothing about how the lexer actually spells what
// it produces -- which is the whole reason this stage exists.
//
// `start` is the token index to begin at, for fixtures that need surrounding
// context the expression parser cannot itself consume: `x: list[int]` is the
// only way to make the scanner spell `int` as TYPE_INT.
inline ParseResult parse_from(const std::string& source, std::size_t start,
                              Entry entry = Entry::Expression) {
    // A separate sink, checked empty: a fixture that accidentally indents
    // would otherwise mix an IndentationError into the parser's diagnostics
    // and make an error test pass for the wrong reason.
    diagnostics::DiagnosticSink indentation_sink;
    lexer::TokenStream stream = lexer::IndentationPass().run(
        lexer::TokenStream(lexer::Lexer(source).tokenize()), indentation_sink);
    EXPECT_TRUE(indentation_sink.empty()) << "fixture produced indentation diagnostics: " << source;
    stream.seek(start);

    diagnostics::DiagnosticSink sink;
    ExpressionParser parser(stream, sink);

    ParseResult result;
    switch (entry) {
        case Entry::Expression:
            result.expression = parser.parse_expression();
            break;
        case Entry::ExpressionList:
            result.expression = parser.parse_expression_list();
            break;
        case Entry::Target:
            result.expression = parser.parse_target();
            break;
    }
    result.diagnostics = sink.diagnostics();
    return result;
}

inline ParseResult parse(const std::string& source) { return parse_from(source, 0); }

// The printed tree of a parse that must succeed cleanly. The two EXPECTs
// here mean a test that asserts only the string still fails loudly if the
// parse reported a diagnostic on the way to the right answer.
inline std::string printed(const std::string& source) {
    const ParseResult result = parse(source);
    EXPECT_TRUE(result.succeeded()) << "parse failed: " << source;
    EXPECT_TRUE(result.diagnostics.empty()) << "unexpected diagnostics: " << source;
    return result.printed();
}

// The printed tree of a parse_expression_list() that must succeed cleanly.
inline std::string printed_list(const std::string& source) {
    const ParseResult result = parse_from(source, 0, Entry::ExpressionList);
    EXPECT_TRUE(result.succeeded()) << "parse failed: " << source;
    EXPECT_TRUE(result.diagnostics.empty()) << "unexpected diagnostics: " << source;
    return result.printed();
}

// The printed tree of a parse_target() that must succeed cleanly.
inline std::string printed_target(const std::string& source) {
    const ParseResult result = parse_from(source, 0, Entry::Target);
    EXPECT_TRUE(result.succeeded()) << "parse failed: " << source;
    EXPECT_TRUE(result.diagnostics.empty()) << "unexpected diagnostics: " << source;
    return result.printed();
}

// The single diagnostic a failing parse must produce. Asserts the count, so
// a rule that reports twice cannot hide behind a message match.
inline diagnostics::Diagnostic only_error(const ParseResult& result) {
    EXPECT_FALSE(result.succeeded()) << "expected the parse to fail";
    EXPECT_EQ(result.diagnostics.size(), 1u) << "expected exactly one diagnostic";
    if (result.diagnostics.size() != 1) {
        return diagnostics::Diagnostic{diagnostics::Severity::Error, "", "", 0, 0};
    }
    return result.diagnostics.front();
}

} // namespace test_support
} // namespace cythonpp::domain::parser

#endif // CYTHONPP_TESTS_DOMAIN_PARSER_PARSE_TEST_HELPERS_H
