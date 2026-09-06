#include <gtest/gtest.h>

#include <string_view>
#include <vector>

#include "domain/lexer/keyword_table.h"
#include "domain/lexer/token_category.h"

namespace cythonpp::domain::lexer {
namespace {

const std::vector<std::string_view>& reserved_words() {
    static const std::vector<std::string_view> words = {
        "False", "None",   "True",  "and",      "as",   "assert", "async", "await", "break",
        "class", "continue", "def", "del",      "elif", "else",   "except", "finally", "for",
        "from",  "global", "if",    "import",   "in",   "is",     "lambda", "nonlocal", "not",
        "or",    "pass",   "raise", "return",   "try",  "while",  "with",  "yield",
    };
    return words;
}

TEST(KeywordTable, EveryReservedWordMapsToItsToken) {
    EXPECT_EQ(reserved_words().size(), 35u);

    EXPECT_EQ(reserved_keyword_of("False"), token_type::BOOL_FALSE);
    EXPECT_EQ(reserved_keyword_of("None"), token_type::KEYWORD_NONE);
    EXPECT_EQ(reserved_keyword_of("True"), token_type::BOOL_TRUE);
    EXPECT_EQ(reserved_keyword_of("and"), token_type::OP_AND);
    EXPECT_EQ(reserved_keyword_of("def"), token_type::KEYWORD_DEF);
    EXPECT_EQ(reserved_keyword_of("import"), token_type::KEYWORD_IMPORT);
    EXPECT_EQ(reserved_keyword_of("in"), token_type::OP_IN);
    EXPECT_EQ(reserved_keyword_of("is"), token_type::OP_IS);
    EXPECT_EQ(reserved_keyword_of("nonlocal"), token_type::KEYWORD_NONLOCAL);
    EXPECT_EQ(reserved_keyword_of("yield"), token_type::KEYWORD_YIELD);

    for (std::string_view word : reserved_words()) {
        EXPECT_TRUE(reserved_keyword_of(word).has_value()) << "missing reserved word " << word;
    }
}

TEST(KeywordTable, EveryReservedWordProducesAKeywordCategorisedToken) {
    for (std::string_view word : reserved_words()) {
        const std::optional<token_type> type = reserved_keyword_of(word);
        ASSERT_TRUE(type.has_value()) << word;
        EXPECT_TRUE(has_category(*type, token_category::KEYWORD)) << word;
    }
}

TEST(KeywordTable, ReservedWordLookupIsCaseSensitive) {
    EXPECT_FALSE(reserved_keyword_of("IF").has_value());
    EXPECT_FALSE(reserved_keyword_of("none").has_value());
    EXPECT_FALSE(reserved_keyword_of("true").has_value());
    EXPECT_FALSE(reserved_keyword_of("False ").has_value());
    EXPECT_FALSE(reserved_keyword_of("").has_value());
}

TEST(KeywordTable, ReservedWordsAreNotBuiltinTypeNames) {
    EXPECT_FALSE(builtin_type_of("if").has_value());
    EXPECT_FALSE(builtin_type_of("None").has_value());
    EXPECT_FALSE(builtin_type_of("class").has_value());
}

TEST(KeywordTable, BuiltinTypeNamesAreNotReservedWords) {
    EXPECT_FALSE(reserved_keyword_of("int").has_value());
    EXPECT_FALSE(reserved_keyword_of("list").has_value());
    EXPECT_FALSE(reserved_keyword_of("object").has_value());

    EXPECT_EQ(builtin_type_of("bool"), token_type::TYPE_BOOL);
    EXPECT_EQ(builtin_type_of("bytearray"), token_type::TYPE_BYTEARRAY);
    EXPECT_EQ(builtin_type_of("bytes"), token_type::TYPE_BYTES);
    EXPECT_EQ(builtin_type_of("complex"), token_type::TYPE_COMPLEX);
    EXPECT_EQ(builtin_type_of("dict"), token_type::TYPE_DICT);
    EXPECT_EQ(builtin_type_of("float"), token_type::TYPE_FLOAT);
    EXPECT_EQ(builtin_type_of("frozenset"), token_type::TYPE_FROZENSET);
    EXPECT_EQ(builtin_type_of("int"), token_type::TYPE_INT);
    EXPECT_EQ(builtin_type_of("list"), token_type::TYPE_LIST);
    EXPECT_EQ(builtin_type_of("object"), token_type::TYPE_OBJECT);
    EXPECT_EQ(builtin_type_of("set"), token_type::TYPE_SET);
    EXPECT_EQ(builtin_type_of("str"), token_type::TYPE_STRING);
    EXPECT_EQ(builtin_type_of("tuple"), token_type::TYPE_TUPLE);
}

TEST(KeywordTable, SoftKeywordsAreNotReservedWords) {
    EXPECT_FALSE(reserved_keyword_of("match").has_value());
    EXPECT_FALSE(reserved_keyword_of("case").has_value());
    EXPECT_FALSE(reserved_keyword_of("_").has_value());

    EXPECT_EQ(soft_keyword_of("_"), token_type::KEYWORD_UNDERSCORE);
    EXPECT_EQ(soft_keyword_of("case"), token_type::KEYWORD_CASE);
    EXPECT_EQ(soft_keyword_of("match"), token_type::KEYWORD_MATCH);
}

// Pins the decision that typing-module names are ordinary identifiers:
// `Optional = 5` is legal Python, so the semantic phase must resolve them
// from the import graph rather than the lexer inventing tokens for them.
TEST(KeywordTable, TypingModuleNamesAreNotRecognised) {
    for (std::string_view word : {"Optional", "Union", "List", "Any", "Callable", "type"}) {
        EXPECT_FALSE(reserved_keyword_of(word).has_value()) << word;
        EXPECT_FALSE(builtin_type_of(word).has_value()) << word;
        EXPECT_FALSE(soft_keyword_of(word).has_value()) << word;
    }
}

TEST(KeywordTable, ClassifyWordFallsBackToIdentifier) {
    EXPECT_EQ(classify_word("counter", false), token_type::IDENTIFIER);
    EXPECT_EQ(classify_word("counter", true), token_type::IDENTIFIER);
    // Also proves no out-of-bounds read on an empty view.
    EXPECT_EQ(classify_word("", false), token_type::IDENTIFIER);
}

TEST(KeywordTable, ClassifyWordEmitsBuiltinTypesOnlyInAnnotationPosition) {
    EXPECT_EQ(classify_word("int", false), token_type::IDENTIFIER);
    EXPECT_EQ(classify_word("int", true), token_type::TYPE_INT);
    EXPECT_EQ(classify_word("list", false), token_type::IDENTIFIER);
    EXPECT_EQ(classify_word("list", true), token_type::TYPE_LIST);
    EXPECT_EQ(classify_word("str", false), token_type::IDENTIFIER);
    EXPECT_EQ(classify_word("str", true), token_type::TYPE_STRING);
    EXPECT_EQ(classify_word("bytes", false), token_type::IDENTIFIER);
    EXPECT_EQ(classify_word("bytes", true), token_type::TYPE_BYTES);
}

TEST(KeywordTable, ClassifyWordPrefersReservedWordsOverAnnotationContext) {
    EXPECT_EQ(classify_word("None", true), token_type::KEYWORD_NONE);
    EXPECT_EQ(classify_word("class", true), token_type::KEYWORD_CLASS);
    EXPECT_EQ(classify_word("True", true), token_type::BOOL_TRUE);
}

TEST(KeywordTable, ClassifyWordNeverEmitsSoftKeywords) {
    EXPECT_EQ(classify_word("match", false), token_type::IDENTIFIER);
    EXPECT_EQ(classify_word("match", true), token_type::IDENTIFIER);
    EXPECT_EQ(classify_word("case", false), token_type::IDENTIFIER);
    EXPECT_EQ(classify_word("_", false), token_type::IDENTIFIER);
}

TEST(KeywordTable, LexemeLookupRoundTripsThroughTheTable) {
    EXPECT_EQ(keyword_lexeme_of(token_type::KEYWORD_IMPORT), "import");
    EXPECT_EQ(keyword_lexeme_of(token_type::OP_IS), "is");
    EXPECT_EQ(keyword_lexeme_of(token_type::BOOL_TRUE), "True");
    EXPECT_EQ(keyword_lexeme_of(token_type::TYPE_STRING), "str");
    EXPECT_EQ(keyword_lexeme_of(token_type::KEYWORD_MATCH), "match");
    EXPECT_TRUE(keyword_lexeme_of(token_type::OP_PLUS).empty());
}

TEST(KeywordTable, CompoundOperatorsSpellThemselvesForDiagnostics) {
    EXPECT_EQ(keyword_lexeme_of(token_type::OP_IS_NOT), "is not");
    EXPECT_EQ(keyword_lexeme_of(token_type::OP_NOT_IN), "not in");
}

TEST(KeywordTable, CompoundOperatorsAreNotWordsTheScannerCanFind) {
    // Reverse lookup only. Joining RESERVED_KEYWORDS would change what
    // reserved_keyword_of() means, since neither string is one word.
    EXPECT_FALSE(reserved_keyword_of("is not").has_value());
    EXPECT_FALSE(reserved_keyword_of("not in").has_value());
    EXPECT_FALSE(soft_keyword_of("not in").has_value());
    EXPECT_EQ(classify_word("not in", false), token_type::IDENTIFIER);
}

} // namespace
} // namespace cythonpp::domain::lexer
