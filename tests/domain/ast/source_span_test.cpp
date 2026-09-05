#include <gtest/gtest.h>

#include <memory>
#include <vector>

#include "domain/ast/bin_op.h"
#include "domain/ast/constant.h"
#include "domain/ast/name.h"
#include "domain/ast/source_span.h"
#include "domain/lexer/lexer.h"
#include "domain/lexer/token.h"

namespace cythonpp::domain::ast {
namespace {

TEST(SourceSpan, SpanOfATokenTakesItsStartAndEnd) {
    const lexer::Token token(lexer::token_type::IDENTIFIER, "total", 3, 5, 3, 10);
    const SourceSpan span = span_of(token);
    EXPECT_EQ(span.start_line, 3);
    EXPECT_EQ(span.start_column, 5);
    EXPECT_EQ(span.end_line, 3);
    EXPECT_EQ(span.end_column, 10);
}

TEST(SourceSpan, MergeTakesTheStartOfTheFirstAndTheEndOfTheLast) {
    const SourceSpan first{1, 1, 1, 2};
    const SourceSpan last{4, 7, 4, 12};
    const SourceSpan merged = merge(first, last);
    EXPECT_EQ(merged.start_line, 1);
    EXPECT_EQ(merged.start_column, 1);
    EXPECT_EQ(merged.end_line, 4);
    EXPECT_EQ(merged.end_column, 12);
}

TEST(SourceSpan, MergingASpanWithItselfIsThatSpan) {
    const SourceSpan span{2, 3, 2, 9};
    EXPECT_EQ(merge(span, span), span);
}

TEST(SourceSpan, EqualityComparesAllFourFields) {
    EXPECT_EQ((SourceSpan{1, 2, 3, 4}), (SourceSpan{1, 2, 3, 4}));
    EXPECT_FALSE((SourceSpan{1, 2, 3, 4}) == (SourceSpan{1, 2, 3, 5}));
}

TEST(SourceSpan, InequalityDetectsEachFieldIndependently) {
    // Common baseline; each case below differs from it in exactly one field.
    const SourceSpan baseline{1, 2, 3, 4};
    EXPECT_FALSE(baseline == (SourceSpan{9, 2, 3, 4})); // start_line differs
    EXPECT_FALSE(baseline == (SourceSpan{1, 9, 3, 4})); // start_column differs
    EXPECT_FALSE(baseline == (SourceSpan{1, 2, 9, 4})); // end_line differs
    EXPECT_FALSE(baseline == (SourceSpan{1, 2, 3, 9})); // end_column differs
}

TEST(SourceSpan, InequalityComparesAllFourFields) {
    // Equal spans should NOT be unequal
    EXPECT_FALSE((SourceSpan{1, 2, 3, 4}) != (SourceSpan{1, 2, 3, 4}));
    // Different in start_line and end_column should be unequal
    EXPECT_TRUE((SourceSpan{1, 2, 3, 4}) != (SourceSpan{2, 2, 3, 5}));
}

// Exercises the real chain the design spec promises: Lexer -> span_of(Token)
// -> merge -> node -> span(). Everything else in this file uses hand-written
// spans; this one pins the half-open convention surviving the trip from the
// lexer's cursor to a node's span(), with concrete expected numbers rather
// than just internal consistency.
TEST(SourceSpan, MergedSpanFromRealLexerTokensBuildsANodeWithTheRightSpan) {
    const std::vector<lexer::Token> tokens = lexer::Lexer("x + 1").tokenize();
    // "x", "+", "1", the synthesized NEWLINE, then TOKEN_EOF -- mid-line
    // whitespace is not tokenized, so there is nothing else in between.
    ASSERT_EQ(tokens.size(), 5u);

    const SourceSpan name_span = span_of(tokens[0]);      // "x"
    const SourceSpan constant_span = span_of(tokens[2]);  // "1"
    const SourceSpan merged = merge(name_span, constant_span);

    const BinOp node(merged, lexer::token_type::OP_PLUS,
                     std::make_unique<Name>(name_span, "x"),
                     std::make_unique<Constant>(constant_span, lexer::token_type::LITERAL_INT,
                                                "1"));

    EXPECT_EQ(node.span(), (SourceSpan{1, 1, 1, 6}));
}

} // namespace
} // namespace cythonpp::domain::ast
