#include "operator_table.h"

#include <algorithm>
#include <array>
#include <optional>

namespace cythonpp::domain::lexer {

namespace {

struct OperatorEntry {
    std::string_view text;
    token_type       type;
};

// The longest punctuation-spelled token in Python is three characters
// ('**=', '//=', '<<=', '>>=', '...'), so trying lengths 3, 2, 1 and taking
// the first hit is exact maximal munch at bounded cost.
constexpr std::size_t MAX_OPERATOR_LENGTH = 3;

// Hand-ordered by ASCII. Watch the '<'/'>' asymmetry: '=' (61) sorts before
// '>' (62), so ">=" precedes ">>", but '<' (60) sorts before '=', so "<<"
// precedes "<=".
//
// Deliberately absent, and not to be "fixed":
//   '!'  alone is not a Python token -- only "!=" exists, so a lone '!'
//        yields length 0 and the scanner reports it.
//   '#'  starts a comment, whose extent is not a fixed string; the scanner
//        must check for it before consulting this table.
//   '?'  has no Python spelling, so token_type::QUESTION is unreachable.
//   '\\' is line continuation, handled by the scanner.
//   '\'' '"' belong to the string scanner.
//   ".." is correctly not an entry: maximal munch finds "." twice.
constexpr std::array<OperatorEntry, 47> OPERATOR_TABLE = {{
    {"!=", token_type::OP_NOT_EQUAL},
    {"%", token_type::OP_PERCENT},
    {"%=", token_type::OP_PERCENT_ASSIGN},
    {"&", token_type::OP_AMPERSAND},
    {"&=", token_type::OP_AMPERSAND_ASSIGN},
    {"(", token_type::OPEN_PAREN},
    {")", token_type::CLOSE_PAREN},
    {"*", token_type::OP_STAR},
    {"**", token_type::OP_DOUBLE_STAR},
    {"**=", token_type::OP_DOUBLE_STAR_ASSIGN},
    {"*=", token_type::OP_STAR_ASSIGN},
    {"+", token_type::OP_PLUS},
    {"+=", token_type::OP_PLUS_ASSIGN},
    {",", token_type::COMMA},
    {"-", token_type::OP_MINUS},
    {"-=", token_type::OP_MINUS_ASSIGN},
    {"->", token_type::OP_ARROW},
    {".", token_type::DOT},
    {"...", token_type::ELLIPSIS},
    {"/", token_type::OP_SLASH},
    {"//", token_type::OP_DOUBLE_SLASH},
    {"//=", token_type::OP_DOUBLE_SLASH_ASSIGN},
    {"/=", token_type::OP_SLASH_ASSIGN},
    {":", token_type::COLON},
    {":=", token_type::OP_WALRUS},
    {";", token_type::SEMICOLON},
    {"<", token_type::OP_LESS},
    {"<<", token_type::OP_LEFT_SHIFT},
    {"<<=", token_type::OP_LEFT_SHIFT_ASSIGN},
    {"<=", token_type::OP_LESS_EQUAL},
    {"=", token_type::OP_ASSIGN},
    {"==", token_type::OP_EQUAL},
    {">", token_type::OP_GREATER},
    {">=", token_type::OP_GREATER_EQUAL},
    {">>", token_type::OP_RIGHT_SHIFT},
    {">>=", token_type::OP_RIGHT_SHIFT_ASSIGN},
    {"@", token_type::OP_AT},
    {"@=", token_type::OP_AT_ASSIGN},
    {"[", token_type::OPEN_BRACKET},
    {"]", token_type::CLOSE_BRACKET},
    {"^", token_type::OP_CARET},
    {"^=", token_type::OP_CARET_ASSIGN},
    {"{", token_type::OPEN_BRACE},
    {"|", token_type::OP_PIPE},
    {"|=", token_type::OP_PIPE_ASSIGN},
    {"}", token_type::CLOSE_BRACE},
    {"~", token_type::OP_TILDE},
}};

// Strict '<' proves ordering and uniqueness in one pass, at build time.
constexpr bool is_strictly_sorted() {
    for (std::size_t index = 1; index < OPERATOR_TABLE.size(); ++index) {
        if (!(OPERATOR_TABLE[index - 1].text < OPERATOR_TABLE[index].text)) {
            return false;
        }
    }
    return true;
}
static_assert(is_strictly_sorted(), "OPERATOR_TABLE must be sorted and duplicate-free");

// MAX_OPERATOR_LENGTH bounds the descending search in
// longest_operator_at(); a longer entry would be silently unreachable.
constexpr bool all_entries_fit_max_length() {
    for (const OperatorEntry& entry : OPERATOR_TABLE) {
        if (entry.text.empty() || entry.text.size() > MAX_OPERATOR_LENGTH) {
            return false;
        }
    }
    return true;
}
static_assert(all_entries_fit_max_length(), "OPERATOR_TABLE entry exceeds MAX_OPERATOR_LENGTH");

// Derived from the table at compile time rather than written out, so adding
// an entry with a new leading character cannot leave this stale.
constexpr std::array<bool, 256> make_operator_start_table() {
    std::array<bool, 256> table{};
    for (const OperatorEntry& entry : OPERATOR_TABLE) {
        table[static_cast<unsigned char>(entry.text.front())] = true;
    }
    return table;
}
constexpr std::array<bool, 256> OPERATOR_START_TABLE = make_operator_start_table();

std::optional<token_type> find_operator(std::string_view text) {
    const auto position = std::lower_bound(OPERATOR_TABLE.begin(), OPERATOR_TABLE.end(), text,
                                           [](const OperatorEntry& entry, std::string_view key) {
                                               return entry.text < key;
                                           });
    if (position == OPERATOR_TABLE.end() || position->text != text) {
        return std::nullopt;
    }
    return position->type;
}

} // namespace

OperatorMatch longest_operator_at(std::string_view text) {
    // Clamping to text.size() up front is what makes end-of-buffer safe.
    for (std::size_t length = std::min(MAX_OPERATOR_LENGTH, text.size()); length > 0; --length) {
        if (const std::optional<token_type> type = find_operator(text.substr(0, length))) {
            return {*type, length};
        }
    }
    return {token_type::TOKEN_ERROR, 0};
}

bool is_operator_start(char c) {
    return OPERATOR_START_TABLE[static_cast<unsigned char>(c)];
}

std::string_view operator_lexeme_of(token_type type) {
    for (const OperatorEntry& entry : OPERATOR_TABLE) {
        if (entry.type == type) {
            return entry.text;
        }
    }
    return {};
}

} // namespace cythonpp::domain::lexer
