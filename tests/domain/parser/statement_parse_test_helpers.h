#ifndef CYTHONPP_TESTS_DOMAIN_PARSER_STATEMENT_PARSE_TEST_HELPERS_H
#define CYTHONPP_TESTS_DOMAIN_PARSER_STATEMENT_PARSE_TEST_HELPERS_H

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "domain/ast/ast_printer.h"
#include "domain/ast/module.h"
#include "domain/diagnostics/diagnostic.h"
#include "domain/diagnostics/diagnostic_sink.h"
#include "domain/lexer/indentation_pass.h"
#include "domain/lexer/lexer.h"
#include "domain/lexer/token_stream.h"
#include "domain/parser/statement_parser.h"

namespace cythonpp::domain::parser {
namespace statement_test_support {

// Everything a statement-parser test asserts on. Owns the module, so a test
// can dynamic_cast into the body when the printed form is not enough --
// AstPrinter erases Constant::type(), so literal tests must read it directly.
struct ModuleResult {
    std::unique_ptr<ast::Module> module;
    std::vector<diagnostics::Diagnostic> diagnostics;
    // Diagnostics IndentationPass produced, kept apart from the parser's so a
    // fixture that accidentally mis-indents cannot make a parser error test
    // pass for the wrong reason.
    std::vector<diagnostics::Diagnostic> indentation_diagnostics;

    std::string printed() const {
        return module == nullptr ? std::string() : ast::AstPrinter().print(*module);
    }
};

// One parse through the real chain: source text -> Lexer -> IndentationPass
// -> StatementParser. Nothing is hand-constructed, because a hand-built token
// vector would prove nothing about how the lexer actually spells what it
// produces -- which is the whole reason this stage exists.
inline ModuleResult parse_module(const std::string& source) {
    ModuleResult result;

    diagnostics::DiagnosticSink indentation_sink;
    lexer::TokenStream stream = lexer::IndentationPass().run(
        lexer::TokenStream(lexer::Lexer(source).tokenize()), indentation_sink);
    result.indentation_diagnostics = indentation_sink.diagnostics();

    diagnostics::DiagnosticSink sink;
    result.module = StatementParser(stream, sink).parse_module();
    result.diagnostics = sink.diagnostics();
    return result;
}

// The printed tree of a parse that must succeed cleanly. The EXPECTs here
// mean a test asserting only the string still fails loudly if the parse
// reported a diagnostic on the way to the right answer.
inline std::string printed(const std::string& source) {
    const ModuleResult result = parse_module(source);
    EXPECT_TRUE(result.indentation_diagnostics.empty())
        << "fixture produced indentation diagnostics: " << source;
    EXPECT_NE(result.module, nullptr) << "parse_module returned null: " << source;
    EXPECT_TRUE(result.diagnostics.empty()) << "unexpected diagnostics: " << source;
    return result.printed();
}

// The single diagnostic a failing parse must produce. Asserts the count, so
// a rule that reports twice cannot hide behind a message match. This is the
// only thing that pins "exactly one diagnostic per failed statement".
inline diagnostics::Diagnostic only_error(const ModuleResult& result) {
    EXPECT_TRUE(result.indentation_diagnostics.empty())
        << "fixture produced indentation diagnostics";
    EXPECT_EQ(result.diagnostics.size(), 1u) << "expected exactly one diagnostic";
    if (result.diagnostics.size() != 1) {
        return diagnostics::Diagnostic{diagnostics::Severity::Error, "", "", 0, 0};
    }
    return result.diagnostics.front();
}

} // namespace statement_test_support
} // namespace cythonpp::domain::parser

#endif // CYTHONPP_TESTS_DOMAIN_PARSER_STATEMENT_PARSE_TEST_HELPERS_H
