#ifndef CYTHONPP_TESTS_DOMAIN_CODEGEN_EMITTER_FIXTURE_H
#define CYTHONPP_TESTS_DOMAIN_CODEGEN_EMITTER_FIXTURE_H

#include <memory>
#include <string>

#include <gtest/gtest.h>

#include "domain/ast/module.h"
#include "domain/diagnostics/diagnostic_sink.h"
#include "domain/lexer/indentation_pass.h"
#include "domain/lexer/lexer.h"
#include "domain/parser/statement_parser.h"
#include "domain/semantic/type_checker.h"
#include "domain/semantic/type_map.h"

namespace cythonpp::domain::codegen {

// Holds everything the emitted text refers to alive for the caller's
// lifetime: the Module owns every Expr, and the TypeMap keys on Expr
// ADDRESSES, so letting the Module die invalidates the map silently.
struct Fixture {
    std::unique_ptr<ast::Module> module;
    semantic::TypeMap types;
    diagnostics::DiagnosticSink emit_sink;
};

// Builds through the REAL chain -- Lexer, IndentationPass, StatementParser,
// TypeChecker -- never hand-constructed AST nodes. A hand-built tree lets a
// test agree with a wrong belief about what the parser produces.
inline Fixture build(const std::string& source) {
    Fixture fixture;
    lexer::Lexer lexer(source);
    const lexer::TokenStream lexed(lexer.tokenize());
    diagnostics::DiagnosticSink front_sink;
    lexer::TokenStream tokens = lexer::IndentationPass().run(lexed, front_sink);
    fixture.module = parser::StatementParser(tokens, front_sink).parse_module();
    EXPECT_TRUE(front_sink.empty()) << "fixture must lex and parse cleanly";
    diagnostics::DiagnosticSink check_sink;
    fixture.types = semantic::TypeChecker(check_sink).check(*fixture.module);
    EXPECT_TRUE(check_sink.empty()) << "fixture must type-check cleanly";
    return fixture;
}

} // namespace cythonpp::domain::codegen

#endif // CYTHONPP_TESTS_DOMAIN_CODEGEN_EMITTER_FIXTURE_H
