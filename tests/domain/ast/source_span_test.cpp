#include <gtest/gtest.h>

#include "domain/ast/source_span.h"
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

} // namespace
} // namespace cythonpp::domain::ast
