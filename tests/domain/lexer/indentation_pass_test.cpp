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

TEST(IndentationPass, UnmatchedDedentReportsIndentationErrorAndContinues) {
    diagnostics::DiagnosticSink sink;
    const TypeList types = types_of("if a:\n    x\n  y\n", sink);

    ASSERT_EQ(sink.diagnostics().size(), 1u);
    EXPECT_EQ(sink.diagnostics().front().code, "IndentationError");
    EXPECT_EQ(sink.diagnostics().front().message, "unindent does not match any outer indentation level");
    EXPECT_EQ(sink.diagnostics().front().line, 3);
    // Scanning did not stop: the statement on the bad line is still in the
    // stream, so a later pass can keep reporting problems in this file.
    // Three identifiers -- the `a` in the header, then `x` and `y`.
    EXPECT_EQ(count_of(types, token_type::IDENTIFIER), 3);
}

TEST(IndentationPass, DedentingPastTheBaseLevelDoesNotUnbalanceTheStream) {
    // The regression case for sentinel overwriting. Line 3 dedents to column 2,
    // which matches no open level; if the base level were overwritten with 2,
    // line 4 at column 0 would pop it and emit a second DEDENT against a single
    // INDENT -- or pop an empty stack.
    diagnostics::DiagnosticSink sink;
    const TypeList types = types_of("if a:\n    x\n  y\nz\n", sink);

    EXPECT_EQ(count_of(types, token_type::INDENT), 1);
    EXPECT_EQ(count_of(types, token_type::DEDENT), 1);
    EXPECT_EQ(sink.diagnostics().size(), 1u);
}

TEST(IndentationPass, RepeatedDedentsPastTheBaseLevelStayBalanced) {
    diagnostics::DiagnosticSink sink;
    const TypeList types = types_of("if a:\n        x\n    y\n        z\n  w\n", sink);

    EXPECT_EQ(count_of(types, token_type::INDENT), 2);
    EXPECT_EQ(count_of(types, token_type::DEDENT), 2);
    EXPECT_EQ(sink.diagnostics().size(), 2u);
}

TEST(IndentationPass, DedentToAColumnBetweenTwoOpenLevelsAdoptsThatColumn) {
    // Column 6 sits between the open levels 4 and 8. One DEDENT is emitted for
    // level 8, then level 4 is overwritten with 6 -- a width change, not a
    // depth change, so the flush at end of file still balances.
    diagnostics::DiagnosticSink sink;
    const TypeList types = types_of("if a:\n    x\n        y\n      z\n", sink);

    EXPECT_EQ(count_of(types, token_type::INDENT), 2);
    EXPECT_EQ(count_of(types, token_type::DEDENT), 2);
    ASSERT_EQ(sink.diagnostics().size(), 1u);
    EXPECT_EQ(sink.diagnostics().front().code, "IndentationError");
}

TEST(IndentationPass, StreamWithoutAnEndOfFileTokenIsStillFlushed) {
    // Not something Lexer produces, but the balance guarantee is unconditional
    // and a hand-built stream must not be able to break it.
    std::vector<Token> tokens;
    tokens.emplace_back(token_type::SPACE, " ", 1, 1);
    tokens.emplace_back(token_type::SPACE, " ", 1, 2);
    tokens.emplace_back(token_type::IDENTIFIER, "x", 1, 3);

    diagnostics::DiagnosticSink sink;
    const TokenStream result = IndentationPass().run(TokenStream(std::move(tokens)), sink);

    TypeList types;
    for (const Token& token : result) {
        types.push_back(token.type());
    }
    EXPECT_EQ(types, (TypeList{token_type::INDENT, token_type::IDENTIFIER, token_type::DEDENT}));
}

TEST(IndentationPass, DeeplyNestedThenFullyDedentedFileStaysBalanced) {
    const TypeList types =
        types_of("if a:\n    if b:\n        if c:\n            if d:\n                x\ny\n");
    EXPECT_EQ(count_of(types, token_type::INDENT), 4);
    EXPECT_EQ(count_of(types, token_type::DEDENT), 4);
}

TEST(IndentationPass, BlankLinesBetweenStatementsDoNotChangeTheLevel) {
    const TypeList types = types_of("if a:\n    x\n\n\n    y\n");
    EXPECT_EQ(count_of(types, token_type::INDENT), 1);
    EXPECT_EQ(count_of(types, token_type::DEDENT), 1);
}

TEST(IndentationPass, CommentOnlyLineAtColumnZeroInsideABlockDoesNotDedent) {
    // The comment line emits a bare COMMENT_SINGLE with no whitespace and no
    // NEWLINE. Treating it as a logical line would read it as a dedent to
    // column 0 and close the block early.
    const TypeList types = types_of("if a:\n    x\n# note\n    y\n");
    EXPECT_EQ(count_of(types, token_type::INDENT), 1);
    EXPECT_EQ(count_of(types, token_type::DEDENT), 1);
    EXPECT_EQ(count_of(types, token_type::COMMENT_SINGLE), 1);
}

TEST(IndentationPass, DedentIsInsertedAfterACommentThatPrecedesTheDedentingLine) {
    // INDENT/DEDENT go immediately before the first *significant* token, so
    // trivia stays where the scanner put it.
    EXPECT_EQ(types_of("if a:\n    x\n# note\ny\n"),
              (TypeList{token_type::KEYWORD_IF, token_type::IDENTIFIER, token_type::COLON,
                        token_type::NEWLINE, token_type::INDENT, token_type::IDENTIFIER,
                        token_type::NEWLINE, token_type::COMMENT_SINGLE, token_type::DEDENT,
                        token_type::IDENTIFIER, token_type::NEWLINE, token_type::TOKEN_EOF}));
}

TEST(IndentationPass, DedentsReachEndOfFileEvenAfterATrailingCommentLine) {
    // The flush anchor is the TOKEN_EOF token, not "after the last NEWLINE".
    const TypeList types = types_of("if a:\n    x\n# note\n");
    ASSERT_GE(types.size(), 3u);
    EXPECT_EQ(types[types.size() - 1], token_type::TOKEN_EOF);
    EXPECT_EQ(types[types.size() - 2], token_type::DEDENT);
    EXPECT_EQ(types[types.size() - 3], token_type::COMMENT_SINGLE);
}

TEST(IndentationPass, ContinuationLineInsideBracketsIsNotMeasured) {
    // No NEWLINE and no whitespace tokens are emitted inside brackets, so the
    // whole call is one logical line at column 0.
    const TypeList types = types_of("f(a,\n        b)\n");
    EXPECT_EQ(count_of(types, token_type::INDENT), 0);
    EXPECT_EQ(count_of(types, token_type::DEDENT), 0);
}

TEST(IndentationPass, BackslashContinuationMeasuresTheFirstPhysicalLine) {
    // The whitespace run is on line 2 and the token it describes is on line 3,
    // so grouping a line by Token::line_number() instead of by adjacency would
    // lose the measurement entirely.
    diagnostics::DiagnosticSink sink;
    const TokenStream result = pass("if a:\n    \\\n    x\n", sink);

    ASSERT_GE(result.size(), 5u);
    const Token& indent = result.at(4);
    EXPECT_EQ(indent.type(), token_type::INDENT);
    EXPECT_EQ(indent.line_number(), 3);
    EXPECT_TRUE(sink.empty());
}

TEST(IndentationPass, SemicolonSeparatedStatementsAreOneLogicalLine) {
    const TypeList types = types_of("if a:\n    x = 1; y = 2\n");
    EXPECT_EQ(count_of(types, token_type::INDENT), 1);
    EXPECT_EQ(count_of(types, token_type::NEWLINE), 2);
}

TEST(IndentationPass, InlineBlockBodyProducesNoIndent) {
    const TypeList types = types_of("if a: x = 1\n");
    EXPECT_EQ(count_of(types, token_type::INDENT), 0);
}

TEST(IndentationPass, IndentationAfterAMultiLineStringIsMeasuredCorrectly) {
    // The scanner's line counter advances through the newlines inside the
    // string, so the run on the line after it carries the right position.
    const TypeList types = types_of("def f():\n    \"\"\"doc\n    more\n    \"\"\"\n    return 1\n");
    EXPECT_EQ(count_of(types, token_type::INDENT), 1);
    EXPECT_EQ(count_of(types, token_type::DEDENT), 1);
}

TEST(IndentationPass, UnterminatedBracketSuppressesIndentationWithoutUnbalancing) {
    const TypeList types = types_of("x = (\n    y\n");
    EXPECT_EQ(count_of(types, token_type::INDENT), 0);
    EXPECT_EQ(count_of(types, token_type::DEDENT), 0);
}

TEST(IndentationPass, UnterminatedTripleQuotedStringLeavesABalancedStream) {
    const TypeList types = types_of("if a:\n    \"\"\"oops\n    x\n");
    EXPECT_EQ(count_of(types, token_type::INDENT), count_of(types, token_type::DEDENT));
}

TEST(IndentationPass, CarriageReturnOnlyLineEndingsCollapseToOneLogicalLine) {
    // Known lexer limitation, pinned rather than worked around: a bare '\r' is
    // swallowed as inter-token whitespace and never re-arms the line start, so
    // no NEWLINE and no indentation tokens exist for such a file.
    const TypeList types = types_of("if x:\r    y\r");
    EXPECT_EQ(count_of(types, token_type::INDENT), 0);
    EXPECT_EQ(count_of(types, token_type::NEWLINE), 1);
}

TEST(IndentationPass, FormFeedBeforeIndentationDivergesFromCPython) {
    // Known deviation, pinned so it is visible rather than forgotten. The
    // scanner's emit loop accepts only ' ' and '\t' while the surrounding skip
    // loops also consume '\f', so the indentation after a leading form feed is
    // absent from the stream and no implementation of this pass can recover it.
    // CPython resets the column on a form feed and would measure column 4 here.
    const TypeList types = types_of("if a:\n\f    x\n");
    EXPECT_EQ(count_of(types, token_type::INDENT), 0);
}

TEST(IndentationPass, FormFeedAfterIndentationDivergesFromCPython) {
    // Same cause, opposite direction: the four spaces before the form feed are
    // counted, where CPython would count only the two after it.
    const TypeList types = types_of("if a:\n    \f  x\n");
    EXPECT_EQ(count_of(types, token_type::INDENT), 1);
}

} // namespace
} // namespace cythonpp::domain::lexer
