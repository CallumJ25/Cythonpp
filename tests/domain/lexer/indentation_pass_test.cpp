#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "domain/diagnostics/diagnostic_sink.h"
#include "domain/lexer/indentation_pass.h"
#include "domain/lexer/lexer.h"
#include "domain/lexer/token_type_name.h"

namespace cythonpp::domain::lexer {
namespace {

using TypeList = std::vector<token_type>;

int count_of(const TypeList& types, token_type wanted) {
    return static_cast<int>(std::count(types.begin(), types.end(), wanted));
}

// The invariant the parser will depend on. Checked on every fixture in this
// file, including the malformed ones, because the recovery paths are exactly
// where a regression would hide and no individual expectation would catch it.
void expect_balanced(const TypeList& types) {
    EXPECT_EQ(count_of(types, token_type::INDENT), count_of(types, token_type::DEDENT))
        << "INDENT/DEDENT counts differ";
    EXPECT_EQ(count_of(types, token_type::SPACE), 0) << "SPACE survived the pass";
    EXPECT_EQ(count_of(types, token_type::TAB), 0) << "TAB survived the pass";
}

TokenStream pass(const std::string& source, diagnostics::DiagnosticSink& sink) {
    return IndentationPass().run(TokenStream(Lexer(source).tokenize()), sink);
}

// For fixtures that are expected to produce diagnostics.
TypeList types_of(const std::string& source, diagnostics::DiagnosticSink& sink) {
    TypeList types;
    for (const Token& token : pass(source, sink)) {
        types.push_back(token.type());
    }
    expect_balanced(types);
    return types;
}

// Runs the pass and asserts the source produced no diagnostics. Most fixtures
// are well-formed, so this keeps them to one line.
TypeList types_of(const std::string& source) {
    diagnostics::DiagnosticSink sink;
    const TypeList types = types_of(source, sink);
    EXPECT_TRUE(sink.empty()) << "unexpected diagnostic count: " << sink.diagnostics().size();
    return types;
}

TEST(IndentationPass, EmptyStreamPassesThroughUnchanged) {
    diagnostics::DiagnosticSink sink;
    const TokenStream result = IndentationPass().run(TokenStream(), sink);

    EXPECT_TRUE(result.empty());
    EXPECT_TRUE(sink.empty());
}

TEST(IndentationPass, UnindentedFileGainsNoIndentOrDedent) {
    EXPECT_EQ(types_of("x = 1\n"),
              (TypeList{token_type::IDENTIFIER, token_type::OP_ASSIGN, token_type::LITERAL_INT,
                        token_type::NEWLINE, token_type::TOKEN_EOF}));
}

TEST(IndentationPass, WhitespaceOnlyFileGainsNoIndentOrDedent) {
    EXPECT_EQ(types_of("   \n\t\n  "), (TypeList{token_type::TOKEN_EOF}));
}

TEST(IndentationPass, CommentOnlyFileKeepsTheCommentAndGainsNothing) {
    EXPECT_EQ(types_of("# note\n"), (TypeList{token_type::COMMENT_SINGLE, token_type::TOKEN_EOF}));
}

TEST(IndentationPass, LeadingWhitespaceIsReplacedByIndentBeforeTheFirstSignificantToken) {
    // The INDENT sits after the NEWLINE that opened the line, not before it,
    // so a parser reading "NEWLINE then INDENT" sees a block start.
    EXPECT_EQ(types_of("if x:\n    y\n"),
              (TypeList{token_type::KEYWORD_IF, token_type::IDENTIFIER, token_type::COLON,
                        token_type::NEWLINE, token_type::INDENT, token_type::IDENTIFIER,
                        token_type::NEWLINE, token_type::DEDENT, token_type::TOKEN_EOF}));
}

TEST(IndentationPass, OneIndentIsEmittedPerLevelRegardlessOfWidth) {
    // A jump of eight columns is one level, not eight.
    EXPECT_EQ(count_of(types_of("if x:\n        y\n"), token_type::INDENT), 1);
}

TEST(IndentationPass, DedentToColumnZeroIsDetectedDespiteNoWhitespaceTokensExisting) {
    // The dedenting line has no SPACE tokens at all, so the comparison has to
    // run once per logical line rather than only when a run was observed.
    EXPECT_EQ(types_of("if x:\n    y\nz\n"),
              (TypeList{token_type::KEYWORD_IF, token_type::IDENTIFIER, token_type::COLON,
                        token_type::NEWLINE, token_type::INDENT, token_type::IDENTIFIER,
                        token_type::NEWLINE, token_type::DEDENT, token_type::IDENTIFIER,
                        token_type::NEWLINE, token_type::TOKEN_EOF}));
}

TEST(IndentationPass, DedentingSeveralLevelsAtOnceEmitsOneDedentPerLevel) {
    const TypeList types = types_of("if a:\n    if b:\n        c\nd\n");
    EXPECT_EQ(count_of(types, token_type::INDENT), 2);
    EXPECT_EQ(count_of(types, token_type::DEDENT), 2);
}

TEST(IndentationPass, EqualIndentationEmitsNeitherIndentNorDedent) {
    const TypeList types = types_of("if a:\n    x\n    y\n    z\n");
    EXPECT_EQ(count_of(types, token_type::INDENT), 1);
    EXPECT_EQ(count_of(types, token_type::DEDENT), 1);
}

TEST(IndentationPass, AllOpenLevelsAreFlushedAsDedentsBeforeEndOfFile) {
    const TypeList types = types_of("if a:\n    if b:\n        c\n");
    EXPECT_EQ(count_of(types, token_type::DEDENT), 2);
    ASSERT_GE(types.size(), 3u);
    EXPECT_EQ(types[types.size() - 1], token_type::TOKEN_EOF);
    EXPECT_EQ(types[types.size() - 2], token_type::DEDENT);
    EXPECT_EQ(types[types.size() - 3], token_type::DEDENT);
}

TEST(IndentationPass, IndentedFirstLineOfTheFileIsMeasured) {
    // There is no preceding NEWLINE, so a loop keyed on "just saw a NEWLINE"
    // would miss this line's indentation entirely.
    const TypeList types = types_of("    x = 1\n");
    EXPECT_EQ(count_of(types, token_type::INDENT), 1);
    EXPECT_EQ(types.front(), token_type::INDENT);
}

TEST(IndentationPass, SynthesizedTokensCarryThePositionOfTheTokenTheyPrecede) {
    diagnostics::DiagnosticSink sink;
    const TokenStream result = pass("if x:\n    y\n", sink);

    const Token& indent = result.at(4);
    ASSERT_EQ(indent.type(), token_type::INDENT);
    EXPECT_EQ(indent.line_number(), 2);
    EXPECT_EQ(indent.column_number(), 5);
    // Synthesized, so an empty lexeme -- the same marker the scanner uses for
    // the NEWLINE it invents at end of file.
    EXPECT_EQ(indent.lexeme(), "");
}

TEST(IndentationPass, RunningTheSamePassObjectTwiceProducesTheSameResult) {
    const IndentationPass indentation;
    const TokenStream lexed(Lexer("if a:\n    x\n").tokenize());

    diagnostics::DiagnosticSink first_sink;
    diagnostics::DiagnosticSink second_sink;
    const TokenStream first = indentation.run(lexed, first_sink);
    const TokenStream second = indentation.run(lexed, second_sink);

    EXPECT_EQ(first.size(), second.size());
    EXPECT_TRUE(first_sink.empty());
    EXPECT_TRUE(second_sink.empty());
}

TEST(IndentationPass, ConsistentTabIndentationProducesNoDiagnostic) {
    const TypeList types = types_of("if a:\n\tx\n\t\ty\n");
    EXPECT_EQ(count_of(types, token_type::INDENT), 2);
    EXPECT_EQ(count_of(types, token_type::DEDENT), 2);
}

TEST(IndentationPass, TabAdvancesToTheNextTabStopRatherThanAddingEight) {
    // "  \t" is column 8. If a tab added eight instead it would be column 10,
    // and the eight-space line below would compare as a dedent rather than as
    // the same column -- so the TabError here is what proves the arithmetic.
    diagnostics::DiagnosticSink sink;
    const TypeList types = types_of("if a:\n  \tx\n        y\n", sink);

    EXPECT_EQ(count_of(types, token_type::INDENT), 1);
    ASSERT_EQ(sink.diagnostics().size(), 1u);
    EXPECT_EQ(sink.diagnostics().front().code, "TabError");
}

TEST(IndentationPass, EqualColumnsWithADifferentTabAndSpaceMixReportTabError) {
    // "\t" and eight spaces are both column 8, but alt column 1 and 8. Which
    // block the second line belongs to depends on the tab width, so Python
    // refuses to guess.
    diagnostics::DiagnosticSink sink;
    types_of("if a:\n\tx\n        y\n", sink);

    ASSERT_EQ(sink.diagnostics().size(), 1u);
    EXPECT_EQ(sink.diagnostics().front().code, "TabError");
    EXPECT_EQ(sink.diagnostics().front().severity, diagnostics::Severity::Error);
    EXPECT_EQ(sink.diagnostics().front().line, 3);
}

TEST(IndentationPass, DeeperColumnWithNonIncreasingAltColumnReportsTabError) {
    // Eight spaces is col 8 / alt 8; two tabs is col 16 / alt 2. Deeper by one
    // measure, shallower by the other.
    diagnostics::DiagnosticSink sink;
    types_of("if a:\n        x\n\t\ty\n", sink);

    ASSERT_GE(sink.diagnostics().size(), 1u);
    EXPECT_EQ(sink.diagnostics().front().code, "TabError");
}

TEST(IndentationPass, TabErrorRecoveryStillEmitsABalancedStream) {
    // expect_balanced runs inside types_of, so this asserts the recovery path
    // does not leak a level. Named explicitly because it is the property, not
    // the token counts, that matters here.
    diagnostics::DiagnosticSink sink;
    const TypeList types = types_of("if a:\n\tx\n        y\n\t\tz\nw\n", sink);
    EXPECT_FALSE(sink.empty());
    EXPECT_EQ(count_of(types, token_type::INDENT), count_of(types, token_type::DEDENT));
}

TEST(IndentationPass, TabErrorDiagnosticsCarryTheOffendingPosition) {
    diagnostics::DiagnosticSink sink;
    types_of("if a:\n\tx\n        y\n", sink);

    ASSERT_EQ(sink.diagnostics().size(), 1u);
    // The position of the token the diagnostic is about, so an editor jumps to
    // the statement rather than to the invisible whitespace before it.
    EXPECT_EQ(sink.diagnostics().front().line, 3);
    EXPECT_EQ(sink.diagnostics().front().column, 9);
}

} // namespace
} // namespace cythonpp::domain::lexer
