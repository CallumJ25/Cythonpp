#include "precedence_table.h"

#include <array>

#include "domain/lexer/token_category.h"

namespace cythonpp::domain::parser {

namespace {

using lexer::token_type;

struct PrecedenceEntry {
    token_type type;
    int        level;
};

// Python's binary levels, loosest first. Every entry is left-associative,
// which is why there is no associativity column: the one right-associative
// operator, '**', is not in this table.
constexpr std::array<PrecedenceEntry, 12> BINARY_OPERATORS = {{
    {token_type::OP_PIPE, 1},
    {token_type::OP_CARET, 2},
    {token_type::OP_AMPERSAND, 3},
    {token_type::OP_LEFT_SHIFT, 4},
    {token_type::OP_RIGHT_SHIFT, 4},
    {token_type::OP_PLUS, 5},
    {token_type::OP_MINUS, 5},
    {token_type::OP_STAR, 6},
    {token_type::OP_AT, 6},
    {token_type::OP_SLASH, 6},
    {token_type::OP_DOUBLE_SLASH, 6},
    {token_type::OP_PERCENT, 6},
}};

} // namespace

std::optional<BinaryPrecedence> binary_precedence_of(lexer::token_type type) {
    // One shift, one mask and one compare, versus a scan of the table. A
    // necessary condition only -- OP_ASSIGN and friends carry OPERATOR too.
    if (!lexer::has_category(type, lexer::token_category::OPERATOR)) {
        return std::nullopt;
    }
    for (const PrecedenceEntry& entry : BINARY_OPERATORS) {
        if (entry.type == type) {
            return BinaryPrecedence{entry.level};
        }
    }
    return std::nullopt;
}

} // namespace cythonpp::domain::parser
