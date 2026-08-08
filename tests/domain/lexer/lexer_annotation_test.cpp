#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "domain/lexer/lexer.h"

namespace cythonpp::domain::lexer {
namespace {

// The type the scanner gave the first occurrence of `word`. Every case below
// hinges on whether one builtin type name resolved as TYPE_* or IDENTIFIER,
// so asking that directly keeps each test to a line or two.
token_type type_of_word(const std::string& source, const std::string& word) {
    for (const Token& token : Lexer(source).tokenize()) {
        if (token.lexeme() == word) {
            return token.type();
        }
    }
    return token_type::TOKEN_ERROR;
}

TEST(LexerAnnotation, AnnotatedAssignmentLexesBuiltinTypeAsATypeToken) {
    EXPECT_EQ(type_of_word("x: int = 5\n", "int"), token_type::TYPE_INT);
}

TEST(LexerAnnotation, BuiltinTypeNameUsedAsAConstructorIsAnIdentifier) {
    EXPECT_EQ(type_of_word("int(\"5\")\n", "int"), token_type::IDENTIFIER);
    EXPECT_EQ(type_of_word("y = str(5)\n", "str"), token_type::IDENTIFIER);
}

TEST(LexerAnnotation, ParameterAnnotationsLexAsTypeTokens) {
    const std::string source = "def f(a: int, b: str = \"x\") -> bool:\n    pass\n";
    EXPECT_EQ(type_of_word(source, "int"), token_type::TYPE_INT);
    EXPECT_EQ(type_of_word(source, "str"), token_type::TYPE_STRING);
    EXPECT_EQ(type_of_word(source, "bool"), token_type::TYPE_BOOL);
}

TEST(LexerAnnotation, ReturnAnnotationAfterArrowLexesAsATypeToken) {
    EXPECT_EQ(type_of_word("def f() -> list:\n    pass\n", "list"), token_type::TYPE_LIST);
}

TEST(LexerAnnotation, DefaultValueAfterAnAnnotationLeavesAnnotationContext) {
    // `int` is the annotation; `str` is a call in the default value.
    const std::string source = "def f(a: int = str(1)):\n    pass\n";
    EXPECT_EQ(type_of_word(source, "int"), token_type::TYPE_INT);
    EXPECT_EQ(type_of_word(source, "str"), token_type::IDENTIFIER);
}

TEST(LexerAnnotation, CommaBetweenParametersEndsAnnotationContext) {
    // `b` is a bare parameter, so the `list` after the comma is a call.
    const std::string source = "def f(a: int, b = list()):\n    pass\n";
    EXPECT_EQ(type_of_word(source, "int"), token_type::TYPE_INT);
    EXPECT_EQ(type_of_word(source, "list"), token_type::IDENTIFIER);
}

TEST(LexerAnnotation, SliceColonDoesNotStartAnnotationContext) {
    EXPECT_EQ(type_of_word("a[1:int]\n", "int"), token_type::IDENTIFIER);
}

TEST(LexerAnnotation, DictLiteralColonDoesNotStartAnnotationContext) {
    EXPECT_EQ(type_of_word("{\"k\": int}\n", "int"), token_type::IDENTIFIER);
}

TEST(LexerAnnotation, GenericSubscriptPropagatesAnnotationContext) {
    const std::string source = "x: list[dict[str, int]] = {}\n";
    EXPECT_EQ(type_of_word(source, "list"), token_type::TYPE_LIST);
    EXPECT_EQ(type_of_word(source, "dict"), token_type::TYPE_DICT);
    EXPECT_EQ(type_of_word(source, "str"), token_type::TYPE_STRING);
    EXPECT_EQ(type_of_word(source, "int"), token_type::TYPE_INT);
}

TEST(LexerAnnotation, CommaInsideAGenericSubscriptDoesNotEndAnnotationContext) {
    EXPECT_EQ(type_of_word("x: dict[str, int]\n", "int"), token_type::TYPE_INT);
}

TEST(LexerAnnotation, LambdaColonDoesNotStartAnnotationContext) {
    EXPECT_EQ(type_of_word("f = lambda a: int\n", "int"), token_type::IDENTIFIER);
    EXPECT_EQ(type_of_word("g(lambda a: str)\n", "str"), token_type::IDENTIFIER);
}

TEST(LexerAnnotation, NestedLambdasResolveTheirOwnColons) {
    EXPECT_EQ(type_of_word("f = lambda a: lambda b: int\n", "int"), token_type::IDENTIFIER);
}

TEST(LexerAnnotation, BlockColonDoesNotStartAnnotationContext) {
    EXPECT_EQ(type_of_word("if x:\n    int(1)\n", "int"), token_type::IDENTIFIER);
    EXPECT_EQ(type_of_word("while x:\n    list()\n", "list"), token_type::IDENTIFIER);
    EXPECT_EQ(type_of_word("try:\n    set()\n", "set"), token_type::IDENTIFIER);
}

// A base class is an expression, not an annotation -- the case naive
// implementations get wrong.
TEST(LexerAnnotation, ClassBaseListIsNotAnnotationContext) {
    EXPECT_EQ(type_of_word("class C(int):\n    pass\n", "int"), token_type::IDENTIFIER);
}

TEST(LexerAnnotation, CallArgumentsInsideABlockHeaderAreNotAnnotationContext) {
    EXPECT_EQ(type_of_word("for i in range(int(n)):\n    pass\n", "int"), token_type::IDENTIFIER);
}

TEST(LexerAnnotation, AnnotationContextEndsAtNewline) {
    EXPECT_EQ(type_of_word("x: bool\nint(1)\n", "int"), token_type::IDENTIFIER);
}

TEST(LexerAnnotation, AnnotationContextEndsAtSemicolon) {
    EXPECT_EQ(type_of_word("x: bool; int(1)\n", "int"), token_type::IDENTIFIER);
}

// The block-header flag is consumed by the first depth-zero colon, so the
// second colon on the line is correctly an annotation colon.
TEST(LexerAnnotation, SecondColonOnACompoundHeaderLineIsAnAnnotationColon) {
    EXPECT_EQ(type_of_word("if cond: x: int = 5\n", "int"), token_type::TYPE_INT);
}

TEST(LexerAnnotation, AnnotationOnASubscriptedTargetStillEntersAnnotationContext) {
    EXPECT_EQ(type_of_word("d[\"k\"]: int = 5\n", "int"), token_type::TYPE_INT);
}

TEST(LexerAnnotation, AnnotationOnAnAttributeTargetStillEntersAnnotationContext) {
    EXPECT_EQ(type_of_word("self.count: int = 0\n", "int"), token_type::TYPE_INT);
}

TEST(LexerAnnotation, NoneIsAlwaysAKeywordEvenInAnnotationPosition) {
    EXPECT_EQ(type_of_word("x: None = None\n", "None"), token_type::KEYWORD_NONE);
}

} // namespace
} // namespace cythonpp::domain::lexer
