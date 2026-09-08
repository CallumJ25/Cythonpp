#include <memory>
#include <string>

#include <gtest/gtest.h>

#include "domain/ast/module.h"
#include "domain/diagnostics/diagnostic_sink.h"
#include "domain/lexer/indentation_pass.h"
#include "domain/lexer/lexer.h"
#include "domain/lexer/token_stream.h"
#include "domain/parser/statement_parser.h"
#include "domain/semantic/type_checker.h"
#include "domain/semantic/type_map.h"
#include "domain/semantic/typed_printer.h"

namespace cythonpp::domain::semantic {
namespace {

// The whole pipeline, so the printer is tested against a map the real
// checker produced rather than a hand-built one.
std::string typed_tree(const std::string& source) {
    lexer::Lexer lexer(source);
    const lexer::TokenStream lexed(lexer.tokenize());
    diagnostics::DiagnosticSink parse_sink;
    lexer::TokenStream tokens = lexer::IndentationPass().run(lexed, parse_sink);
    const std::unique_ptr<ast::Module> module =
        parser::StatementParser(tokens, parse_sink).parse_module();
    EXPECT_TRUE(parse_sink.empty());

    diagnostics::DiagnosticSink sink;
    const TypeMap types = TypeChecker(sink).check(*module);
    EXPECT_TRUE(sink.empty()) << "fixture must check cleanly";
    return TypedPrinter().print(*module, types);
}

TEST(TypedPrinter, AppendsTheTypeToEveryTypedExpression) {
    EXPECT_EQ(typed_tree("x: int = 5\nx + 1\n"),
              "(Module\n"
              "  (AnnAssign (Name x) (Name int) (Constant 5):int)\n"
              "  (ExprStmt (BinOp + (Name x):int (Constant 1):int):int))");
}

// The property that makes sharing the map beat rebuilding it: annotations
// have no entries, so they render bare with NO special case.
TEST(TypedPrinter, AnnotationSubtreesRenderBare) {
    const std::string tree = typed_tree("x: list[int] = []\n");

    EXPECT_NE(tree.find("(Name list)"), std::string::npos)
        << "the annotation must carry no type suffix";
    EXPECT_EQ(tree.find("(Name list):"), std::string::npos);
}

// AstPrinter erases Constant::type() -- LITERAL_INT "1" and LITERAL_FLOAT "1"
// both render (Constant 1). The suffix is what disambiguates them, which is
// exactly why this oracle exists.
TEST(TypedPrinter, DisambiguatesLiteralsThatAstPrinterRendersIdentically) {
    EXPECT_NE(typed_tree("1\n").find("(Constant 1):int"), std::string::npos);
    EXPECT_NE(typed_tree("1.0\n").find("(Constant 1.0):float"), std::string::npos);
}

TEST(TypedPrinter, RendersNestedTypes) {
    EXPECT_NE(typed_tree("x: dict[str, int] = {}\n").find("):dict[str, int]"),
              std::string::npos);
}

TEST(TypedPrinter, RendersAnEmptyModule) {
    EXPECT_EQ(typed_tree(""), "(Module)");
}

} // namespace
} // namespace cythonpp::domain::semantic
