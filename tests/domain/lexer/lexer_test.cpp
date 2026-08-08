#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "domain/lexer/lexer.h"
#include "domain/lexer/token_type_name.h"

namespace cythonpp::domain::lexer {
namespace {

std::vector<Token> lex(const std::string& source) { return Lexer(source).tokenize(); }

std::vector<token_type> types_of(const std::string& source) {
    std::vector<token_type> types;
    for (const Token& token : lex(source)) {
        types.push_back(token.type());
    }
    return types;
}

// The type of the first token, so single-literal cases read in one line.
token_type first_type(const std::string& source) { return lex(source).front().type(); }

std::string first_lexeme(const std::string& source) { return lex(source).front().lexeme(); }

TEST(Lexer, EmptySourceProducesOnlyEndOfFileToken) {
    const std::vector<Token> tokens = lex("");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type(), token_type::TOKEN_EOF);
    EXPECT_EQ(tokens[0].line_number(), 1);
    EXPECT_EQ(tokens[0].column_number(), 1);
}

TEST(Lexer, SourceWithoutTrailingNewlineStillGetsNewlineBeforeEndOfFile) {
    EXPECT_EQ(types_of("x"),
              (std::vector<token_type>{token_type::IDENTIFIER, token_type::NEWLINE, token_type::TOKEN_EOF}));
    // The synthesized newline is marked by an empty lexeme.
    EXPECT_EQ(lex("x")[1].lexeme(), "");
}

TEST(Lexer, TrailingNewlineDoesNotProduceASecondNewlineToken) {
    EXPECT_EQ(types_of("x\n"),
              (std::vector<token_type>{token_type::IDENTIFIER, token_type::NEWLINE, token_type::TOKEN_EOF}));
    EXPECT_EQ(lex("x\n")[1].lexeme(), "\n");
}

TEST(Lexer, BlankLinesProduceNoTokens) {
    EXPECT_EQ(types_of("\n\n\n"), (std::vector<token_type>{token_type::TOKEN_EOF}));
    EXPECT_EQ(types_of("x\n\n\ny\n"),
              (std::vector<token_type>{token_type::IDENTIFIER, token_type::NEWLINE, token_type::IDENTIFIER,
                                       token_type::NEWLINE, token_type::TOKEN_EOF}));
    // The gap is still recoverable from the line numbers.
    EXPECT_EQ(lex("x\n\n\ny\n")[2].line_number(), 4);
}

TEST(Lexer, CommentOnlyLineProducesCommentButNoNewline) {
    EXPECT_EQ(types_of("# just a note\n"),
              (std::vector<token_type>{token_type::COMMENT_SINGLE, token_type::TOKEN_EOF}));
}

TEST(Lexer, WhitespaceOnlyFileProducesOnlyEndOfFileToken) {
    EXPECT_EQ(types_of("   \n\t\n  "), (std::vector<token_type>{token_type::TOKEN_EOF}));
}

TEST(Lexer, LeadingSpacesProduceOneSpaceTokenPerCharacter) {
    const std::vector<Token> tokens = lex("if x:\n    y\n");
    // if x : NEWLINE + four SPACE + y + NEWLINE + EOF
    ASSERT_EQ(tokens.size(), 11u);
    for (std::size_t index = 4; index < 8; ++index) {
        EXPECT_EQ(tokens[index].type(), token_type::SPACE);
        EXPECT_EQ(tokens[index].lexeme(), " ");
        EXPECT_EQ(tokens[index].line_number(), 2);
        EXPECT_EQ(tokens[index].column_number(), static_cast<int>(index) - 3);
    }
    EXPECT_EQ(tokens[8].type(), token_type::IDENTIFIER);
    EXPECT_EQ(tokens[8].column_number(), 5);
}

TEST(Lexer, LeadingTabsProduceTabTokensDistinctFromSpaces) {
    const std::vector<Token> tokens = lex("if x:\n\ty\n");
    ASSERT_EQ(tokens.size(), 8u);
    EXPECT_EQ(tokens[4].type(), token_type::TAB);
    EXPECT_EQ(tokens[4].lexeme(), "\t");
}

// The whole reason SPACE and TAB are separate tokens: a later pass has to be
// able to apply Python's tab/space rules and report TabError.
TEST(Lexer, MixedLeadingTabsAndSpacesPreserveTheirOrder) {
    const std::vector<Token> tokens = lex("if x:\n\t  \ty\n");
    ASSERT_GE(tokens.size(), 9u);
    EXPECT_EQ(tokens[4].type(), token_type::TAB);
    EXPECT_EQ(tokens[5].type(), token_type::SPACE);
    EXPECT_EQ(tokens[6].type(), token_type::SPACE);
    EXPECT_EQ(tokens[7].type(), token_type::TAB);
    EXPECT_EQ(tokens[8].type(), token_type::IDENTIFIER);
}

TEST(Lexer, WhitespaceBetweenTokensIsNotTokenized) {
    EXPECT_EQ(types_of("x   =    1\n"),
              (std::vector<token_type>{token_type::IDENTIFIER, token_type::OP_ASSIGN, token_type::LITERAL_INT,
                                       token_type::NEWLINE, token_type::TOKEN_EOF}));
}

TEST(Lexer, IndentationOfABlankLineIsNotTokenized) {
    EXPECT_EQ(types_of("x\n    \ny\n"),
              (std::vector<token_type>{token_type::IDENTIFIER, token_type::NEWLINE, token_type::IDENTIFIER,
                                       token_type::NEWLINE, token_type::TOKEN_EOF}));
}

TEST(Lexer, IndentationOfACommentOnlyLineIsNotTokenized) {
    EXPECT_EQ(types_of("x\n    # note\ny\n"),
              (std::vector<token_type>{token_type::IDENTIFIER, token_type::NEWLINE, token_type::COMMENT_SINGLE,
                                       token_type::IDENTIFIER, token_type::NEWLINE, token_type::TOKEN_EOF}));
}

TEST(Lexer, NewlineInsideBracketsEmitsNoNewlineToken) {
    EXPECT_EQ(types_of("f(a,\n  b)\n"),
              (std::vector<token_type>{token_type::IDENTIFIER, token_type::OPEN_PAREN, token_type::IDENTIFIER,
                                       token_type::COMMA, token_type::IDENTIFIER, token_type::CLOSE_PAREN,
                                       token_type::NEWLINE, token_type::TOKEN_EOF}));

    EXPECT_EQ(types_of("[a,\n b]\n"),
              (std::vector<token_type>{token_type::OPEN_BRACKET, token_type::IDENTIFIER, token_type::COMMA,
                                       token_type::IDENTIFIER, token_type::CLOSE_BRACKET, token_type::NEWLINE,
                                       token_type::TOKEN_EOF}));

    EXPECT_EQ(types_of("{a,\n b}\n"),
              (std::vector<token_type>{token_type::OPEN_BRACE, token_type::IDENTIFIER, token_type::COMMA,
                                       token_type::IDENTIFIER, token_type::CLOSE_BRACE, token_type::NEWLINE,
                                       token_type::TOKEN_EOF}));
}

TEST(Lexer, NestedBracketsRestoreNewlineEmissionOnlyAtDepthZero) {
    EXPECT_EQ(types_of("f([a,\n b],\n c)\nx\n"),
              (std::vector<token_type>{token_type::IDENTIFIER, token_type::OPEN_PAREN, token_type::OPEN_BRACKET,
                                       token_type::IDENTIFIER, token_type::COMMA, token_type::IDENTIFIER,
                                       token_type::CLOSE_BRACKET, token_type::COMMA, token_type::IDENTIFIER,
                                       token_type::CLOSE_PAREN, token_type::NEWLINE, token_type::IDENTIFIER,
                                       token_type::NEWLINE, token_type::TOKEN_EOF}));
}

TEST(Lexer, ContinuationLineLeadingWhitespaceIsNotTokenized) {
    for (token_type type : types_of("f(a,\n        b)\n")) {
        EXPECT_NE(type, token_type::SPACE);
        EXPECT_NE(type, token_type::TAB);
    }
}

TEST(Lexer, BackslashAtEndOfLineSuppressesTheNewlineToken) {
    EXPECT_EQ(types_of("x = 1 + \\\n    2\n"),
              (std::vector<token_type>{token_type::IDENTIFIER, token_type::OP_ASSIGN, token_type::LITERAL_INT,
                                       token_type::OP_PLUS, token_type::LITERAL_INT, token_type::NEWLINE,
                                       token_type::TOKEN_EOF}));
}

TEST(Lexer, CommentInsideBracketsDoesNotEndTheLogicalLine) {
    EXPECT_EQ(types_of("f(a,  # why\n  b)\n"),
              (std::vector<token_type>{token_type::IDENTIFIER, token_type::OPEN_PAREN, token_type::IDENTIFIER,
                                       token_type::COMMA, token_type::COMMENT_SINGLE, token_type::IDENTIFIER,
                                       token_type::CLOSE_PAREN, token_type::NEWLINE, token_type::TOKEN_EOF}));
}

TEST(Lexer, KeywordsLexAsTheirKeywordTokenTypes) {
    EXPECT_EQ(first_type("def f(): pass"), token_type::KEYWORD_DEF);
    EXPECT_EQ(first_type("import os"), token_type::KEYWORD_IMPORT);
    EXPECT_EQ(first_type("lambda x: x"), token_type::KEYWORD_LAMBDA);
    EXPECT_EQ(first_type("None"), token_type::KEYWORD_NONE);
    EXPECT_EQ(first_type("True"), token_type::BOOL_TRUE);
    EXPECT_EQ(first_type("not x"), token_type::OP_NOT);
}

TEST(Lexer, IdentifiersThatMerelyStartWithAKeywordAreIdentifiers) {
    EXPECT_EQ(first_type("define"), token_type::IDENTIFIER);
    EXPECT_EQ(first_type("iffy"), token_type::IDENTIFIER);
    EXPECT_EQ(first_type("class_"), token_type::IDENTIFIER);
    EXPECT_EQ(first_type("notation"), token_type::IDENTIFIER);
    EXPECT_EQ(first_type("_private"), token_type::IDENTIFIER);
    EXPECT_EQ(first_type("value2"), token_type::IDENTIFIER);
}

TEST(Lexer, SoftKeywordsAlwaysLexAsIdentifiers) {
    EXPECT_EQ(first_type("match x:\n    pass\n"), token_type::IDENTIFIER);
    EXPECT_EQ(first_type("case _:\n    pass\n"), token_type::IDENTIFIER);
}

TEST(Lexer, IntegerLiteralForms) {
    EXPECT_EQ(first_type("0"), token_type::LITERAL_INT);
    EXPECT_EQ(first_type("42"), token_type::LITERAL_INT);
    EXPECT_EQ(first_type("0xFF"), token_type::LITERAL_INT);
    EXPECT_EQ(first_type("0o755"), token_type::LITERAL_INT);
    EXPECT_EQ(first_type("0b1010"), token_type::LITERAL_INT);
    EXPECT_EQ(first_lexeme("0xDEAD_BEEF"), "0xDEAD_BEEF");
}

TEST(Lexer, UnderscoreSeparatorsAreKeptInTheLexeme) {
    EXPECT_EQ(first_type("1_000_000"), token_type::LITERAL_INT);
    EXPECT_EQ(first_lexeme("1_000_000"), "1_000_000");
}

TEST(Lexer, FloatLiteralForms) {
    EXPECT_EQ(first_type("3.14"), token_type::LITERAL_FLOAT);
    EXPECT_EQ(first_type(".5"), token_type::LITERAL_FLOAT);
    EXPECT_EQ(first_lexeme(".5"), ".5");
    EXPECT_EQ(first_type("5."), token_type::LITERAL_FLOAT);
    EXPECT_EQ(first_type("1e10"), token_type::LITERAL_FLOAT);
    EXPECT_EQ(first_type("1.5E-3"), token_type::LITERAL_FLOAT);
    EXPECT_EQ(first_lexeme("1.5E-3"), "1.5E-3");
    EXPECT_EQ(first_type("1_000.000_1"), token_type::LITERAL_FLOAT);
}

TEST(Lexer, ComplexSuffixIsPartOfTheNumericLiteral) {
    EXPECT_EQ(first_type("3j"), token_type::LITERAL_COMPLEX);
    EXPECT_EQ(first_type("1.5J"), token_type::LITERAL_COMPLEX);
    EXPECT_EQ(first_lexeme("3j"), "3j");
}

// The exponent guard: 'e' only starts an exponent when a digit or a signed
// digit follows, so `1if x else 2` does not mis-scan.
TEST(Lexer, ExponentRequiresDigitsToFollow) {
    EXPECT_EQ(types_of("1if x else 2"),
              (std::vector<token_type>{token_type::LITERAL_INT, token_type::KEYWORD_IF, token_type::IDENTIFIER,
                                       token_type::KEYWORD_ELSE, token_type::LITERAL_INT, token_type::NEWLINE,
                                       token_type::TOKEN_EOF}));
}

TEST(Lexer, EllipsisIsNotScannedAsANumber) {
    EXPECT_EQ(first_type("..."), token_type::ELLIPSIS);
    EXPECT_EQ(first_lexeme("..."), "...");
}

TEST(Lexer, AttributeAccessOnANameIsADotNotAFloat) {
    EXPECT_EQ(types_of("a.b\n"),
              (std::vector<token_type>{token_type::IDENTIFIER, token_type::DOT, token_type::IDENTIFIER,
                                       token_type::NEWLINE, token_type::TOKEN_EOF}));
}

TEST(Lexer, SingleAndDoubleQuotedStringsKeepTheirQuotesInTheLexeme) {
    EXPECT_EQ(first_type("'hi'"), token_type::LITERAL_STRING);
    EXPECT_EQ(first_lexeme("'hi'"), "'hi'");
    EXPECT_EQ(first_lexeme("\"hi\""), "\"hi\"");
}

TEST(Lexer, EscapedQuoteDoesNotTerminateTheString) {
    EXPECT_EQ(first_lexeme("\"a\\\"b\""), "\"a\\\"b\"");
    EXPECT_EQ(types_of("\"a\\\"b\"\n"),
              (std::vector<token_type>{token_type::LITERAL_STRING, token_type::NEWLINE, token_type::TOKEN_EOF}));
}

TEST(Lexer, RawStringBackslashDoesNotEscapeOutOfTheString) {
    EXPECT_EQ(first_lexeme("r\"\\\"\""), "r\"\\\"\"");
    EXPECT_EQ(first_type("r\"\\\"\""), token_type::LITERAL_STRING);
}

TEST(Lexer, TripleQuotedStringSpansMultipleLines) {
    EXPECT_EQ(types_of("\"\"\"a\nb\"\"\"\n"),
              (std::vector<token_type>{token_type::LITERAL_STRING, token_type::NEWLINE, token_type::TOKEN_EOF}));
    EXPECT_EQ(first_lexeme("'''a\nb'''"), "'''a\nb'''");
}

// A docstring is an ordinary string; COMMENT_MULTI is never emitted.
TEST(Lexer, TripleQuotedStringIsALiteralStringNotAMultiLineComment) {
    for (token_type type : types_of("\"\"\"Module docstring.\"\"\"\n")) {
        EXPECT_NE(type, token_type::COMMENT_MULTI);
    }
    EXPECT_EQ(first_type("\"\"\"Module docstring.\"\"\"\n"), token_type::LITERAL_STRING);
}

TEST(Lexer, AllStringPrefixesAreLexedAsPartOfTheStringToken) {
    EXPECT_EQ(first_type("r'a'"), token_type::LITERAL_STRING);
    EXPECT_EQ(first_type("u'a'"), token_type::LITERAL_STRING);
    EXPECT_EQ(first_type("b'a'"), token_type::LITERAL_BYTES);
    EXPECT_EQ(first_type("rb'a'"), token_type::LITERAL_BYTES);
    EXPECT_EQ(first_type("BR'a'"), token_type::LITERAL_BYTES);
    EXPECT_EQ(first_lexeme("rb'a'"), "rb'a'");
}

TEST(Lexer, UnterminatedSingleQuotedStringStopsAtTheNewline) {
    EXPECT_EQ(types_of("'oops\nx\n"),
              (std::vector<token_type>{token_type::LITERAL_STRING, token_type::NEWLINE, token_type::IDENTIFIER,
                                       token_type::NEWLINE, token_type::TOKEN_EOF}));
}

TEST(Lexer, FStringIsLexedIntoPartsWithRealTokensForInterpolations) {
    EXPECT_EQ(types_of("f\"a{x + 1}b\"\n"),
              (std::vector<token_type>{token_type::FSTRING_START, token_type::FSTRING_MIDDLE,
                                       token_type::OPEN_BRACE, token_type::IDENTIFIER, token_type::OP_PLUS,
                                       token_type::LITERAL_INT, token_type::CLOSE_BRACE,
                                       token_type::FSTRING_MIDDLE, token_type::FSTRING_END,
                                       token_type::NEWLINE, token_type::TOKEN_EOF}));
}

// The reason for splitting rather than keeping one opaque token: positions
// inside an interpolation stay relative to the file, so diagnostics point at
// the right place.
TEST(Lexer, FStringInterpolationTokensCarryFileRelativePositions) {
    const std::vector<Token> tokens = lex("y = f\"a{value}\"\n");
    bool found = false;
    for (const Token& token : tokens) {
        if (token.type() == token_type::IDENTIFIER && token.lexeme() == "value") {
            EXPECT_EQ(token.line_number(), 1);
            EXPECT_EQ(token.column_number(), 9);
            found = true;
        }
    }
    EXPECT_TRUE(found);
}

TEST(Lexer, DoubledBracesInAnFStringAreLiteralText) {
    EXPECT_EQ(types_of("f\"{{literal}}\"\n"),
              (std::vector<token_type>{token_type::FSTRING_START, token_type::FSTRING_MIDDLE,
                                       token_type::FSTRING_END, token_type::NEWLINE, token_type::TOKEN_EOF}));
}

TEST(Lexer, FStringFormatSpecIsLiteralTextWithNestedFieldsLexed) {
    EXPECT_EQ(types_of("f\"{value:>{width}}\"\n"),
              (std::vector<token_type>{token_type::FSTRING_START, token_type::OPEN_BRACE, token_type::IDENTIFIER,
                                       token_type::COLON, token_type::FSTRING_MIDDLE, token_type::OPEN_BRACE,
                                       token_type::IDENTIFIER, token_type::CLOSE_BRACE, token_type::CLOSE_BRACE,
                                       token_type::FSTRING_END, token_type::NEWLINE, token_type::TOKEN_EOF}));
}

TEST(Lexer, FStringConversionAndDebugSpecifiersAreFormattingNotOperators) {
    EXPECT_EQ(types_of("f\"{x!r}\"\n"),
              (std::vector<token_type>{token_type::FSTRING_START, token_type::OPEN_BRACE, token_type::IDENTIFIER,
                                       token_type::FSTRING_MIDDLE, token_type::CLOSE_BRACE,
                                       token_type::FSTRING_END, token_type::NEWLINE, token_type::TOKEN_EOF}));

    EXPECT_EQ(types_of("f\"{x=}\"\n"),
              (std::vector<token_type>{token_type::FSTRING_START, token_type::OPEN_BRACE, token_type::IDENTIFIER,
                                       token_type::FSTRING_MIDDLE, token_type::CLOSE_BRACE,
                                       token_type::FSTRING_END, token_type::NEWLINE, token_type::TOKEN_EOF}));
}

TEST(Lexer, ComparisonOperatorsInsideAnFStringStayOperators) {
    EXPECT_EQ(types_of("f\"{a != b}\"\n"),
              (std::vector<token_type>{token_type::FSTRING_START, token_type::OPEN_BRACE, token_type::IDENTIFIER,
                                       token_type::OP_NOT_EQUAL, token_type::IDENTIFIER, token_type::CLOSE_BRACE,
                                       token_type::FSTRING_END, token_type::NEWLINE, token_type::TOKEN_EOF}));
}

TEST(Lexer, NestedBracketsInsideAnFStringFieldDoNotCloseIt) {
    EXPECT_EQ(types_of("f\"{d['k']}\"\n"),
              (std::vector<token_type>{token_type::FSTRING_START, token_type::OPEN_BRACE, token_type::IDENTIFIER,
                                       token_type::OPEN_BRACKET, token_type::LITERAL_STRING,
                                       token_type::CLOSE_BRACKET, token_type::CLOSE_BRACE,
                                       token_type::FSTRING_END, token_type::NEWLINE, token_type::TOKEN_EOF}));
}

TEST(Lexer, MaximalMunchPrefersLongerOperators) {
    EXPECT_EQ(first_type("**= 1"), token_type::OP_DOUBLE_STAR_ASSIGN);
    EXPECT_EQ(first_type("// 1"), token_type::OP_DOUBLE_SLASH);
    EXPECT_EQ(first_type("<= 1"), token_type::OP_LESS_EQUAL);
    EXPECT_EQ(first_type("-> int"), token_type::OP_ARROW);
    EXPECT_EQ(types_of("a := 1\n"),
              (std::vector<token_type>{token_type::IDENTIFIER, token_type::OP_WALRUS, token_type::LITERAL_INT,
                                       token_type::NEWLINE, token_type::TOKEN_EOF}));
}

TEST(Lexer, LineAndColumnNumbersAreOneBased) {
    const std::vector<Token> tokens = lex("x = 1\n");
    EXPECT_EQ(tokens[0].line_number(), 1);
    EXPECT_EQ(tokens[0].column_number(), 1);
    EXPECT_EQ(tokens[1].column_number(), 3);
    EXPECT_EQ(tokens[2].column_number(), 5);
}

TEST(Lexer, ColumnResetsToOneAfterEachNewline) {
    const std::vector<Token> tokens = lex("ab\ncd\n");
    EXPECT_EQ(tokens[2].line_number(), 2);
    EXPECT_EQ(tokens[2].column_number(), 1);
}

TEST(Lexer, PositionAfterATripleQuotedStringAccountsForEmbeddedNewlines) {
    const std::vector<Token> tokens = lex("'''a\nb\nc'''\nx\n");
    // The string token itself starts at 1:1 ...
    EXPECT_EQ(tokens[0].line_number(), 1);
    EXPECT_EQ(tokens[0].column_number(), 1);
    // ... and the name after it is on line 4.
    EXPECT_EQ(tokens[2].type(), token_type::IDENTIFIER);
    EXPECT_EQ(tokens[2].line_number(), 4);
    EXPECT_EQ(tokens[2].column_number(), 1);
}

TEST(Lexer, PositionAfterAContinuationAccountsForTheConsumedNewline) {
    const std::vector<Token> tokens = lex("x = 1 + \\\n2\n");
    EXPECT_EQ(tokens[4].type(), token_type::LITERAL_INT);
    EXPECT_EQ(tokens[4].line_number(), 2);
    EXPECT_EQ(tokens[4].column_number(), 1);
}

// Bytes >= 0x80 are identifier characters, so a non-ASCII name is one token
// rather than a run of errors, and its lexeme is the exact source bytes.
TEST(Lexer, NonAsciiIdentifiersAreASingleTokenWithTheirBytesPreserved) {
    const std::vector<Token> tokens = lex("caf\xc3\xa9 = 1\n");
    EXPECT_EQ(tokens[0].type(), token_type::IDENTIFIER);
    EXPECT_EQ(tokens[0].lexeme(), "caf\xc3\xa9");
}

TEST(Lexer, NonAsciiStringContentsRoundTripExactly) {
    // UTF-8 is self-synchronizing, so no byte of a multi-byte character can
    // be mistaken for the closing quote.
    const std::string source = "s = \"\xe6\x97\xa5\xe6\x9c\xac \xf0\x9f\x90\x8d\"\n";
    const std::vector<Token> tokens = lex(source);
    EXPECT_EQ(tokens[2].type(), token_type::LITERAL_STRING);
    EXPECT_EQ(tokens[2].lexeme(), "\"\xe6\x97\xa5\xe6\x9c\xac \xf0\x9f\x90\x8d\"");
}

TEST(Lexer, ColumnsCountCharactersNotBytes) {
    // "café = 1": é is two bytes, so a byte-counting column would report the
    // '=' at 7 and the literal at 9.
    const std::vector<Token> tokens = lex("caf\xc3\xa9 = 1\n");
    EXPECT_EQ(tokens[0].column_number(), 1);
    EXPECT_EQ(tokens[1].column_number(), 6);
    EXPECT_EQ(tokens[2].column_number(), 8);

    // A four-byte character counts once too.
    const std::vector<Token> emoji = lex("\"\xf0\x9f\x90\x8d\" + x\n");
    EXPECT_EQ(emoji[0].column_number(), 1);
    EXPECT_EQ(emoji[1].column_number(), 5);
    EXPECT_EQ(emoji[2].column_number(), 7);
}

TEST(Lexer, ColumnsResetCorrectlyAfterALineContainingNonAscii) {
    const std::vector<Token> tokens = lex("\xce\xb1 = 1\nx = 2\n");
    EXPECT_EQ(tokens[1].column_number(), 3);
    EXPECT_EQ(tokens[4].line_number(), 2);
    EXPECT_EQ(tokens[4].column_number(), 1);
}

TEST(Lexer, UnrecognizedCharacterProducesErrorTokenAndScanningContinues) {
    EXPECT_EQ(types_of("a $ b\n"),
              (std::vector<token_type>{token_type::IDENTIFIER, token_type::TOKEN_ERROR, token_type::IDENTIFIER,
                                       token_type::NEWLINE, token_type::TOKEN_EOF}));
    EXPECT_EQ(lex("a $ b\n")[1].lexeme(), "$");
}

TEST(Lexer, LoneBangProducesAnErrorTokenButNotEqualsDoesNot) {
    EXPECT_EQ(types_of("a ! b\n"),
              (std::vector<token_type>{token_type::IDENTIFIER, token_type::TOKEN_ERROR, token_type::IDENTIFIER,
                                       token_type::NEWLINE, token_type::TOKEN_EOF}));
    EXPECT_EQ(types_of("a != b\n"),
              (std::vector<token_type>{token_type::IDENTIFIER, token_type::OP_NOT_EQUAL, token_type::IDENTIFIER,
                                       token_type::NEWLINE, token_type::TOKEN_EOF}));
}

TEST(Lexer, HashCommentRunsToEndOfLineOnly) {
    EXPECT_EQ(types_of("x = 1  # trailing\ny\n"),
              (std::vector<token_type>{token_type::IDENTIFIER, token_type::OP_ASSIGN, token_type::LITERAL_INT,
                                       token_type::COMMENT_SINGLE, token_type::NEWLINE, token_type::IDENTIFIER,
                                       token_type::NEWLINE, token_type::TOKEN_EOF}));
    EXPECT_EQ(lex("x = 1  # trailing\n")[3].lexeme(), "# trailing");
}

TEST(Lexer, HashInsideAStringIsNotAComment) {
    EXPECT_EQ(types_of("\"a # b\"\n"),
              (std::vector<token_type>{token_type::LITERAL_STRING, token_type::NEWLINE, token_type::TOKEN_EOF}));
}

TEST(Lexer, SemicolonSeparatesStatementsOnOneLine) {
    EXPECT_EQ(types_of("x = 1; y = 2\n"),
              (std::vector<token_type>{token_type::IDENTIFIER, token_type::OP_ASSIGN, token_type::LITERAL_INT,
                                       token_type::SEMICOLON, token_type::IDENTIFIER, token_type::OP_ASSIGN,
                                       token_type::LITERAL_INT, token_type::NEWLINE, token_type::TOKEN_EOF}));
}

TEST(Lexer, TokenizeIsRepeatable) {
    Lexer lexer("x = 1\n");
    const std::vector<Token> first = lexer.tokenize();
    const std::vector<Token> second = lexer.tokenize();
    ASSERT_EQ(first.size(), second.size());
    for (std::size_t index = 0; index < first.size(); ++index) {
        EXPECT_EQ(first[index].type(), second[index].type());
        EXPECT_EQ(first[index].lexeme(), second[index].lexeme());
    }
}

// The contents of test_files/hello_world.py, inline rather than read from
// disk: domain tests must not touch the filesystem.
TEST(Lexer, HelloWorldSampleLexesToTheExpectedTokenSequence) {
    const std::string source =
        "print(\"Hello World\")\n"
        "\n"
        "string: str = \"Hello\"\n"
        "\n"
        "if string != \"hello\":\n"
        "    print(\"Something has happened to string\")";

    EXPECT_EQ(types_of(source),
              (std::vector<token_type>{
                  token_type::IDENTIFIER, token_type::OPEN_PAREN, token_type::LITERAL_STRING,
                  token_type::CLOSE_PAREN, token_type::NEWLINE,

                  token_type::IDENTIFIER, token_type::COLON, token_type::TYPE_STRING, token_type::OP_ASSIGN,
                  token_type::LITERAL_STRING, token_type::NEWLINE,

                  token_type::KEYWORD_IF, token_type::IDENTIFIER, token_type::OP_NOT_EQUAL,
                  token_type::LITERAL_STRING, token_type::COLON, token_type::NEWLINE,

                  token_type::SPACE, token_type::SPACE, token_type::SPACE, token_type::SPACE,
                  token_type::IDENTIFIER, token_type::OPEN_PAREN, token_type::LITERAL_STRING,
                  token_type::CLOSE_PAREN, token_type::NEWLINE, token_type::TOKEN_EOF}));
}

} // namespace
} // namespace cythonpp::domain::lexer
