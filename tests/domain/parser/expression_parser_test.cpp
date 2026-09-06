#include <gtest/gtest.h>

#include <string>

#include "domain/ast/constant.h"
#include "domain/lexer/token_type.h"
#include "parse_test_helpers.h"

namespace cythonpp::domain::parser {
namespace {

using test_support::parse;
using test_support::printed;

// The token_type of a root Constant. AstPrinter erases it, so a literal-kind
// misclassification is invisible in the printed form alone.
lexer::token_type constant_type(const std::string& source) {
    const test_support::ParseResult result = parse(source);
    EXPECT_TRUE(result.succeeded()) << "parse failed: " << source;
    const auto* constant = dynamic_cast<const ast::Constant*>(result.expression.get());
    EXPECT_NE(constant, nullptr) << "root was not a Constant: " << source;
    return constant == nullptr ? lexer::token_type::TOKEN_ERROR : constant->type();
}

TEST(ExpressionParser, IdentifierBecomesAName) {
    EXPECT_EQ(printed("total"), "(Name total)");
}

TEST(ExpressionParser, LiteralsKeepTheirRawLexeme) {
    EXPECT_EQ(printed("42"), "(Constant 42)");
    EXPECT_EQ(printed("0xFF"), "(Constant 0xFF)");
    EXPECT_EQ(printed("1_000"), "(Constant 1_000)");
    EXPECT_EQ(printed("3.5"), "(Constant 3.5)");
    EXPECT_EQ(printed("'hi'"), "(Constant 'hi')");
}

TEST(ExpressionParser, LiteralsKeepTheirLexerTokenType) {
    EXPECT_EQ(constant_type("42"), lexer::token_type::LITERAL_INT);
    EXPECT_EQ(constant_type("3.5"), lexer::token_type::LITERAL_FLOAT);
    EXPECT_EQ(constant_type("1j"), lexer::token_type::LITERAL_COMPLEX);
    EXPECT_EQ(constant_type("'hi'"), lexer::token_type::LITERAL_STRING);
    EXPECT_EQ(constant_type("b'hi'"), lexer::token_type::LITERAL_BYTES);
    EXPECT_EQ(constant_type("True"), lexer::token_type::BOOL_TRUE);
    EXPECT_EQ(constant_type("False"), lexer::token_type::BOOL_FALSE);
    EXPECT_EQ(constant_type("None"), lexer::token_type::KEYWORD_NONE);
    EXPECT_EQ(constant_type("..."), lexer::token_type::ELLIPSIS);
}

TEST(ExpressionParser, SingletonsPrintTheirSpelling) {
    EXPECT_EQ(printed("True"), "(Constant True)");
    EXPECT_EQ(printed("None"), "(Constant None)");
    EXPECT_EQ(printed("..."), "(Constant ...)");
}

TEST(ExpressionParser, SoftKeywordsAreOrdinaryNames) {
    // match/case/_ are reserved only inside a match statement, which is not
    // in the supported subset. In expression position they are just names,
    // and the scanner already emits IDENTIFIER for all three.
    EXPECT_EQ(printed("match"), "(Name match)");
    EXPECT_EQ(printed("case"), "(Name case)");
    EXPECT_EQ(printed("_"), "(Name _)");
}

TEST(ExpressionParser, BuiltinTypeNamesOutsideAnnotationsAreNames) {
    EXPECT_EQ(printed("int"), "(Name int)");
    EXPECT_EQ(printed("str"), "(Name str)");
}

} // namespace
} // namespace cythonpp::domain::parser
