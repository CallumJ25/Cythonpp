#ifndef CYTHONPP_DOMAIN_PARSER_PRECEDENCE_TABLE_H
#define CYTHONPP_DOMAIN_PARSER_PRECEDENCE_TABLE_H

#include <optional>

#include "domain/lexer/token_type.h"

namespace cythonpp::domain::parser {

// The binding power of a left-associative binary operator: level 1 binds
// loosest ('|'), level 6 tightest ('*', '@', '/', '//', '%').
//
// A struct rather than a bare int so that adding a column later -- a fixity,
// an arity -- does not change the type at every call site.
struct BinaryPrecedence {
    int level;
};

inline constexpr int LOWEST_BINARY_LEVEL = 1;

// The binding power of `type`, or nullopt if it is not a left-associative
// binary operator.
//
// nullopt deliberately covers several tokens that carry
// token_category::OPERATOR: OP_ASSIGN and the aug-assigns, OP_WALRUS,
// OP_ARROW, OP_TILDE and OP_NOT. Category membership is the cheap guard, not
// the answer -- this table is the authority.
//
// Comparison and boolean operators are absent because they build Compare and
// BoolOp rather than BinOp, and '**' because it is right-associative and its
// interaction with unary operators is asymmetric (`-2**2` is `-(2**2)` but
// `2**-1` is legal). Both are hand-written rules in ExpressionParser.
std::optional<BinaryPrecedence> binary_precedence_of(lexer::token_type type);

} // namespace cythonpp::domain::parser

#endif // CYTHONPP_DOMAIN_PARSER_PRECEDENCE_TABLE_H
