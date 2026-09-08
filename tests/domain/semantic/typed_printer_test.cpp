#include <memory>
#include <string>

#include <gtest/gtest.h>

#include "domain/ast/ast_printer.h"
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

// ---------------------------------------------------------------------------
// Differential test: TypedPrinter's bare form must never drift from
// AstPrinter's.
//
// The review's one real finding on this task: adding a 27th ast node fails
// loudly (ast::Visitor's pure virtuals break both renderers at compile time),
// but CHANGING how an existing node renders in AstPrinter does not -- nothing
// forces a matching edit in typed_printer.cpp's mirrored `visit`. Both files
// still compile, AstPrinter's own tests still pass, and every test above
// still passes too, because each asserts its own hardcoded expected string
// rather than cross-checking against AstPrinter. A one-space or one-field
// drift in an unrelated node would go completely unnoticed.
//
// APPROACH CHOSEN: compare TypedPrinter fed an EMPTY TypeMap against
// AstPrinter's output directly, for byte-for-byte equality -- not a
// stripped-suffix comparison against a populated map. TypeMap::find always
// misses on an empty map, so append_suffix (typed_printer.cpp) is
// unconditionally a no-op by construction: there is no suffix for a regex to
// have stripped correctly or incorrectly, so this is EXACT rather than
// heuristic. A stripping regex would also have been genuinely fragile here --
// this codebase's own type names contain the exact characters a suffix
// stripper must not misread as terminators: `dict[str, int]` and `str | None`
// both contain `, ` and ` `, and a `Callable[[int], str]` return type nests
// brackets a naive "run to the next space" rule would truncate mid-type. The
// empty-map comparison sidesteps that class of bug entirely rather than
// trying to out-regex it.
//
// FIXTURE BREADTH: parsed_module() below runs the real Lexer ->
// IndentationPass -> StatementParser chain (never a hand-built tree), and the
// two fixtures together exercise all twenty-six concrete ast node kinds --
// every statement AstPrinter/TypedPrinter can render (AnnAssign, Assign,
// Break, ClassDef, Continue, ExprStmt, For, FunctionDef, If, Module, Pass,
// Return, While) and every expression (Attribute, BinOp, BoolOp, Call,
// Compare, Constant, DictExpr, ListComp, ListExpr, Name, Subscript,
// TupleExpr, UnaryOp) -- plus the `Else` suite shared by If/For/While and a
// handful of optional-field branches (a value-less AnnAssign, a value-less
// Return, a zero-argument Call, an annotated parameter with a return
// annotation, a base-less ClassDef written with empty parens). None of this
// needs to check cleanly -- an empty TypeMap means TypeChecker's own rules
// are irrelevant to this property -- so only the PARSE sink is asserted
// empty.
std::unique_ptr<ast::Module> parsed_module(const std::string& source) {
    lexer::Lexer lexer(source);
    const lexer::TokenStream lexed(lexer.tokenize());
    diagnostics::DiagnosticSink parse_sink;
    lexer::TokenStream tokens = lexer::IndentationPass().run(lexed, parse_sink);
    std::unique_ptr<ast::Module> module =
        parser::StatementParser(tokens, parse_sink).parse_module();
    EXPECT_TRUE(parse_sink.empty()) << "fixture must parse cleanly:\n" << source;
    return module;
}

void expect_bare_rendering_matches_ast_printer(const std::string& source) {
    const std::unique_ptr<ast::Module> module = parsed_module(source);
    ASSERT_NE(module, nullptr);

    const std::string bare_form = TypedPrinter().print(*module, TypeMap());
    const std::string reference = ast::AstPrinter().print(*module);
    EXPECT_EQ(bare_form, reference) << "source:\n" << source;
}

TEST(TypedPrinter, MatchesAstPrinterVerbatimOnAnEmptyTypeMapAcrossEveryNodeKind) {
    expect_bare_rendering_matches_ast_printer(
        "class C(A, B):\n"
        "    x: int = 0\n"
        "    d: dict[str, int] = {}\n"
        "    def m(self, a, b=1):\n"
        "        for i in [1, 2]:\n"
        "            if i > 0:\n"
        "                continue\n"
        "            else:\n"
        "                break\n"
        "        while a:\n"
        "            a = a - 1\n"
        "        y = [n for n in [1, 2] if n]\n"
        "        z = {1: 2, 3: 4}\n"
        "        t = 1, 2\n"
        "        w = self.x\n"
        "        v = -a\n"
        "        u = not True\n"
        "        s = a and b or True\n"
        "        r = a == b < 10\n"
        "        q = self.m(a, b)\n"
        "        p = t[0]\n"
        "        print(q)\n"
        "        return a\n"
        "def f(x: int) -> int:\n"
        "    return x\n");
}

// The shared print_else helper (duplicated verbatim in typed_printer.cpp) and
// a handful of optional-field branches the fixture above never exercises:
// a bare (value-less) AnnAssign, a bare Return, an empty-parens/no-bases
// ClassDef, and a zero-argument Call.
TEST(TypedPrinter, MatchesAstPrinterVerbatimForElseClausesAndOptionalForms) {
    expect_bare_rendering_matches_ast_printer(
        "class D():\n"
        "    pass\n"
        "def g():\n"
        "    n: int\n"
        "    x = 0\n"
        "    while x < 3:\n"
        "        x = x + 1\n"
        "    else:\n"
        "        pass\n"
        "    for i in range(3):\n"
        "        pass\n"
        "    else:\n"
        "        pass\n"
        "    if x:\n"
        "        pass\n"
        "    elif n:\n"
        "        pass\n"
        "    else:\n"
        "        pass\n"
        "    dict()\n"
        "    return\n");
}

} // namespace
} // namespace cythonpp::domain::semantic
