#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "domain/ast/ast_printer.h"
#include "domain/ast/attribute.h"
#include "domain/ast/constant.h"
#include "domain/ast/name.h"
#include "domain/ast/subscript.h"

namespace cythonpp::domain::ast {
namespace {

// Every node in these tests needs a span, and which span is almost never what
// the test is about, so one placeholder keeps the assertions readable.
constexpr SourceSpan kSpan{1, 1, 1, 2};

std::string print(const Node& node) { return AstPrinter().print(node); }

TEST(AstPrinter, NameRendersItsIdentifier) {
    const Name node(kSpan, "total");
    EXPECT_EQ(print(node), "(Name total)");
}

TEST(AstPrinter, ConstantRendersItsRawLexeme) {
    const Constant node(kSpan, lexer::token_type::LITERAL_INT, "0xFF");
    EXPECT_EQ(print(node), "(Constant 0xFF)");
}

TEST(AstPrinter, ConstantKeepsTheLexerTokenType) {
    const Constant node(kSpan, lexer::token_type::LITERAL_STRING, "'hi'");
    EXPECT_EQ(node.type(), lexer::token_type::LITERAL_STRING);
    EXPECT_EQ(print(node), "(Constant 'hi')");
}

TEST(AstPrinter, AttributeRendersValueThenName) {
    const Attribute node(kSpan, std::make_unique<Name>(kSpan, "self"), "count");
    EXPECT_EQ(print(node), "(Attribute (Name self) count)");
}

TEST(AstPrinter, SubscriptRendersValueThenIndex) {
    const Subscript node(kSpan, std::make_unique<Name>(kSpan, "items"),
                         std::make_unique<Constant>(kSpan, lexer::token_type::LITERAL_INT, "0"));
    EXPECT_EQ(print(node), "(Subscript (Name items) (Constant 0))");
}

TEST(Node, CarriesTheSpanItWasBuiltWith) {
    const Name node(SourceSpan{4, 9, 4, 14}, "total");
    EXPECT_EQ(node.span(), (SourceSpan{4, 9, 4, 14}));
}

} // namespace
} // namespace cythonpp::domain::ast
