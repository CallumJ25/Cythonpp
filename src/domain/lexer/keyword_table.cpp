#include "keyword_table.h"

#include <algorithm>
#include <array>
#include <cstddef>

namespace cythonpp::domain::lexer {

namespace {

struct KeywordEntry {
    std::string_view text;
    token_type       type;
};

// Tables are hand-ordered by ASCII, so uppercase sorts before lowercase.
// Binary search is only correct given that ordering, which is why
// is_strictly_sorted() proves it at build time rather than at review time.
constexpr std::array<KeywordEntry, 35> RESERVED_KEYWORDS = {{
    {"False", token_type::BOOL_FALSE},
    {"None", token_type::KEYWORD_NONE},
    {"True", token_type::BOOL_TRUE},
    {"and", token_type::OP_AND},
    {"as", token_type::KEYWORD_AS},
    {"assert", token_type::KEYWORD_ASSERT},
    {"async", token_type::KEYWORD_ASYNC},
    {"await", token_type::KEYWORD_AWAIT},
    {"break", token_type::KEYWORD_BREAK},
    {"class", token_type::KEYWORD_CLASS},
    {"continue", token_type::KEYWORD_CONTINUE},
    {"def", token_type::KEYWORD_DEF},
    {"del", token_type::KEYWORD_DEL},
    {"elif", token_type::KEYWORD_ELIF},
    {"else", token_type::KEYWORD_ELSE},
    {"except", token_type::KEYWORD_EXCEPT},
    {"finally", token_type::KEYWORD_FINALLY},
    {"for", token_type::KEYWORD_FOR},
    {"from", token_type::KEYWORD_FROM},
    {"global", token_type::KEYWORD_GLOBAL},
    {"if", token_type::KEYWORD_IF},
    {"import", token_type::KEYWORD_IMPORT},
    {"in", token_type::OP_IN},
    {"is", token_type::OP_IS},
    {"lambda", token_type::KEYWORD_LAMBDA},
    {"nonlocal", token_type::KEYWORD_NONLOCAL},
    {"not", token_type::OP_NOT},
    {"or", token_type::OP_OR},
    {"pass", token_type::KEYWORD_PASS},
    {"raise", token_type::KEYWORD_RAISE},
    {"return", token_type::KEYWORD_RETURN},
    {"try", token_type::KEYWORD_TRY},
    {"while", token_type::KEYWORD_WHILE},
    {"with", token_type::KEYWORD_WITH},
    {"yield", token_type::KEYWORD_YIELD},
}};

// Only the builtins that name a concrete runtime type. Typing-module names
// (Optional, Union, List, Any, Callable) are deliberately absent: they are
// attributes bound by `from typing import ...`, so `Optional = 5` is legal
// and lexing them specially would be wrong. `type` is absent too -- it is a
// soft keyword introducing PEP 695 type aliases.
constexpr std::array<KeywordEntry, 13> BUILTIN_TYPE_NAMES = {{
    {"bool", token_type::TYPE_BOOL},
    {"bytearray", token_type::TYPE_BYTEARRAY},
    {"bytes", token_type::TYPE_BYTES},
    {"complex", token_type::TYPE_COMPLEX},
    {"dict", token_type::TYPE_DICT},
    {"float", token_type::TYPE_FLOAT},
    {"frozenset", token_type::TYPE_FROZENSET},
    {"int", token_type::TYPE_INT},
    {"list", token_type::TYPE_LIST},
    {"object", token_type::TYPE_OBJECT},
    {"set", token_type::TYPE_SET},
    {"str", token_type::TYPE_STRING},
    {"tuple", token_type::TYPE_TUPLE},
}};

// '_' is ASCII 95, so it sorts ahead of the lowercase words.
constexpr std::array<KeywordEntry, 3> SOFT_KEYWORDS = {{
    {"_", token_type::KEYWORD_UNDERSCORE},
    {"case", token_type::KEYWORD_CASE},
    {"match", token_type::KEYWORD_MATCH},
}};

// Strict '<' proves ordering and uniqueness in one pass: a duplicate key
// compares equal and fails. std::string_view's comparison is constexpr in
// C++17, so this runs entirely at build time.
template <std::size_t N>
constexpr bool is_strictly_sorted(const std::array<KeywordEntry, N>& table) {
    for (std::size_t index = 1; index < N; ++index) {
        if (!(table[index - 1].text < table[index].text)) {
            return false;
        }
    }
    return true;
}

static_assert(is_strictly_sorted(RESERVED_KEYWORDS), "RESERVED_KEYWORDS must be sorted and duplicate-free");
static_assert(is_strictly_sorted(BUILTIN_TYPE_NAMES), "BUILTIN_TYPE_NAMES must be sorted and duplicate-free");
static_assert(is_strictly_sorted(SOFT_KEYWORDS), "SOFT_KEYWORDS must be sorted and duplicate-free");

template <std::size_t N>
std::optional<token_type> find_word(const std::array<KeywordEntry, N>& table, std::string_view word) {
    const auto position = std::lower_bound(table.begin(), table.end(), word,
                                           [](const KeywordEntry& entry, std::string_view key) {
                                               return entry.text < key;
                                           });
    if (position == table.end() || position->text != word) {
        return std::nullopt;
    }
    return position->type;
}

template <std::size_t N>
std::string_view find_lexeme(const std::array<KeywordEntry, N>& table, token_type type) {
    for (const KeywordEntry& entry : table) {
        if (entry.type == type) {
            return entry.text;
        }
    }
    return {};
}

} // namespace

std::optional<token_type> reserved_keyword_of(std::string_view word) {
    return find_word(RESERVED_KEYWORDS, word);
}

std::optional<token_type> builtin_type_of(std::string_view word) {
    return find_word(BUILTIN_TYPE_NAMES, word);
}

std::optional<token_type> soft_keyword_of(std::string_view word) {
    return find_word(SOFT_KEYWORDS, word);
}

token_type classify_word(std::string_view word, bool in_annotation) {
    // Reserved-first ordering is load-bearing: classify_word("None", true)
    // must yield KEYWORD_NONE, never a TYPE_*.
    if (const std::optional<token_type> reserved = reserved_keyword_of(word)) {
        return *reserved;
    }
    if (in_annotation) {
        if (const std::optional<token_type> builtin = builtin_type_of(word)) {
            return *builtin;
        }
    }
    return token_type::IDENTIFIER;
}

std::string_view keyword_lexeme_of(token_type type) {
    if (const std::string_view reserved = find_lexeme(RESERVED_KEYWORDS, type); !reserved.empty()) {
        return reserved;
    }
    if (const std::string_view builtin = find_lexeme(BUILTIN_TYPE_NAMES, type); !builtin.empty()) {
        return builtin;
    }
    return find_lexeme(SOFT_KEYWORDS, type);
}

} // namespace cythonpp::domain::lexer
