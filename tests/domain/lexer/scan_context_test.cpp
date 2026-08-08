#include <gtest/gtest.h>

#include <initializer_list>

#include "domain/lexer/scan_context.h"

namespace cythonpp::domain::lexer {
namespace {

// Feeds a token sequence and reports the annotation state at the end. Driving
// the machine directly, rather than through source strings, is the point of
// having it as its own class.
bool annotation_after(std::initializer_list<token_type> types) {
    ScanContext context;
    for (token_type type : types) {
        context.observe(type);
    }
    return context.in_annotation();
}

TEST(ScanContext, StartsOutsideAnnotationContext) {
    ScanContext context;
    EXPECT_FALSE(context.in_annotation());
    EXPECT_EQ(context.bracket_depth(), 0u);
}

TEST(ScanContext, BracketDepthTracksNesting) {
    ScanContext context;
    context.observe(token_type::OPEN_PAREN);
    EXPECT_EQ(context.bracket_depth(), 1u);
    context.observe(token_type::OPEN_BRACKET);
    EXPECT_EQ(context.bracket_depth(), 2u);
    context.observe(token_type::CLOSE_BRACKET);
    EXPECT_EQ(context.bracket_depth(), 1u);
    context.observe(token_type::CLOSE_PAREN);
    EXPECT_EQ(context.bracket_depth(), 0u);
}

TEST(ScanContext, UnbalancedClosingBracketIsIgnored) {
    ScanContext context;
    context.observe(token_type::CLOSE_PAREN);
    EXPECT_EQ(context.bracket_depth(), 0u);
}

TEST(ScanContext, TopLevelColonAfterANameEntersAnnotationContext) {
    EXPECT_TRUE(annotation_after({token_type::IDENTIFIER, token_type::COLON}));
}

TEST(ScanContext, TopLevelColonOnABlockHeaderLineDoesNotEnterAnnotationContext) {
    EXPECT_FALSE(annotation_after({token_type::KEYWORD_IF, token_type::IDENTIFIER, token_type::COLON}));
    EXPECT_FALSE(annotation_after({token_type::KEYWORD_FOR, token_type::IDENTIFIER, token_type::COLON}));
    EXPECT_FALSE(annotation_after({token_type::KEYWORD_CLASS, token_type::IDENTIFIER, token_type::COLON}));
    EXPECT_FALSE(annotation_after({token_type::KEYWORD_TRY, token_type::COLON}));
}

TEST(ScanContext, ArrowAlwaysEntersAnnotationContext) {
    EXPECT_TRUE(annotation_after({token_type::KEYWORD_DEF, token_type::IDENTIFIER, token_type::OPEN_PAREN,
                                  token_type::CLOSE_PAREN, token_type::OP_ARROW}));
}

TEST(ScanContext, ColonInsideParenthesesIsAParameterAnnotation) {
    EXPECT_TRUE(annotation_after({token_type::KEYWORD_DEF, token_type::IDENTIFIER, token_type::OPEN_PAREN,
                                  token_type::IDENTIFIER, token_type::COLON}));
}

TEST(ScanContext, ColonInsideBracketsIsASlice) {
    EXPECT_FALSE(annotation_after({token_type::IDENTIFIER, token_type::OPEN_BRACKET, token_type::LITERAL_INT,
                                   token_type::COLON}));
}

TEST(ScanContext, ColonInsideBracesIsADictSeparator) {
    EXPECT_FALSE(annotation_after({token_type::OPEN_BRACE, token_type::LITERAL_STRING, token_type::COLON}));
}

TEST(ScanContext, LambdaColonIsConsumedByTheLambda) {
    EXPECT_FALSE(annotation_after({token_type::IDENTIFIER, token_type::OP_ASSIGN, token_type::KEYWORD_LAMBDA,
                                   token_type::IDENTIFIER, token_type::COLON}));
}

TEST(ScanContext, NestedLambdasConsumeTheirOwnColons) {
    EXPECT_FALSE(annotation_after({token_type::KEYWORD_LAMBDA, token_type::IDENTIFIER, token_type::COLON,
                                   token_type::KEYWORD_LAMBDA, token_type::IDENTIFIER, token_type::COLON}));
}

TEST(ScanContext, LambdaInsideParenthesesDoesNotStealTheParameterColon) {
    // f(a: int) -- the lambda is at a different depth and must not match.
    EXPECT_TRUE(annotation_after({token_type::KEYWORD_LAMBDA, token_type::IDENTIFIER, token_type::COLON,
                                  token_type::KEYWORD_DEF, token_type::OPEN_PAREN, token_type::IDENTIFIER,
                                  token_type::COLON}));
}

TEST(ScanContext, AssignmentLeavesAnnotationContext) {
    EXPECT_FALSE(annotation_after({token_type::IDENTIFIER, token_type::COLON, token_type::TYPE_INT,
                                   token_type::OP_ASSIGN}));
}

TEST(ScanContext, CommaLeavesAnnotationContextInsideAParameterList) {
    EXPECT_FALSE(annotation_after({token_type::KEYWORD_DEF, token_type::IDENTIFIER, token_type::OPEN_PAREN,
                                   token_type::IDENTIFIER, token_type::COLON, token_type::TYPE_INT,
                                   token_type::COMMA}));
}

// The one bit per bracket frame that makes commas behave: inside a generic
// subscript opened in annotation context, a comma must not clear the flag.
TEST(ScanContext, CommaKeepsAnnotationContextInsideAGenericSubscript) {
    EXPECT_TRUE(annotation_after({token_type::IDENTIFIER, token_type::COLON, token_type::TYPE_DICT,
                                  token_type::OPEN_BRACKET, token_type::TYPE_STRING, token_type::COMMA}));
}

TEST(ScanContext, ClosingAGenericSubscriptRestoresTheEnclosingAnnotationState) {
    ScanContext context;
    context.observe(token_type::IDENTIFIER);
    context.observe(token_type::COLON);
    EXPECT_TRUE(context.in_annotation());
    context.observe(token_type::TYPE_LIST);
    context.observe(token_type::OPEN_BRACKET);
    EXPECT_TRUE(context.in_annotation());
    context.observe(token_type::TYPE_INT);
    context.observe(token_type::CLOSE_BRACKET);
    EXPECT_TRUE(context.in_annotation());
}

TEST(ScanContext, ClosingACallRestoresTheNonAnnotationState) {
    ScanContext context;
    context.observe(token_type::KEYWORD_CLASS);
    context.observe(token_type::IDENTIFIER);
    context.observe(token_type::OPEN_PAREN);
    EXPECT_FALSE(context.in_annotation());
    context.observe(token_type::CLOSE_PAREN);
    EXPECT_FALSE(context.in_annotation());
}

TEST(ScanContext, NewlineResetsAnnotationContext) {
    EXPECT_FALSE(annotation_after({token_type::IDENTIFIER, token_type::COLON, token_type::NEWLINE}));
}

TEST(ScanContext, SemicolonResetsAnnotationContext) {
    EXPECT_FALSE(annotation_after({token_type::IDENTIFIER, token_type::COLON, token_type::SEMICOLON}));
}

TEST(ScanContext, NewlineAlsoClearsAPendingBlockHeader) {
    // After the newline a fresh line starting with a name gets an annotation
    // colon, proving block_header_pending_ did not leak across the line.
    EXPECT_TRUE(annotation_after({token_type::KEYWORD_IF, token_type::IDENTIFIER, token_type::COLON,
                                  token_type::NEWLINE, token_type::IDENTIFIER, token_type::COLON}));
}

TEST(ScanContext, SecondColonOnABlockHeaderLineIsAnAnnotationColon) {
    EXPECT_TRUE(annotation_after({token_type::KEYWORD_IF, token_type::IDENTIFIER, token_type::COLON,
                                  token_type::IDENTIFIER, token_type::COLON}));
}

TEST(ScanContext, AnnotationOnASubscriptedTargetEntersAnnotationContext) {
    EXPECT_TRUE(annotation_after({token_type::IDENTIFIER, token_type::OPEN_BRACKET, token_type::LITERAL_STRING,
                                  token_type::CLOSE_BRACKET, token_type::COLON}));
}

} // namespace
} // namespace cythonpp::domain::lexer
