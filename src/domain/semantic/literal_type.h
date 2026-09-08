#ifndef CYTHONPP_DOMAIN_SEMANTIC_LITERAL_TYPE_H
#define CYTHONPP_DOMAIN_SEMANTIC_LITERAL_TYPE_H

#include <string>

#include "domain/lexer/token_type.h"
#include "type.h"

namespace cythonpp::domain::semantic {

// The type of a literal, from the token type ast::Constant carries.
//
// Takes only the token type: classification never reads the lexeme, and the
// spec's rationale for the signature -- these are facts about lexer output,
// not about tree shape -- is satisfied without it.
//
// Total. A token type no ast::Constant can carry yields Unknown rather than
// an assertion.
//
// This function does NOT decode. Semantic analysis validates; codegen
// decodes. No int64_t, double, or escape-processed string value appears
// anywhere in domain/semantic/, which is the falsifiable form of that split.
Type literal_type(lexer::token_type type);

// Whether an integer literal's MAGNITUDE is representable in 64 bits, i.e.
// whether it is at most 2^63 (or, when `allow_two_to_63` is false, strictly
// less than 2^63).
//
// 2^63 is accepted BY DEFAULT rather than rejected because the sign is not
// visible here: `-9223372036854775808` is a valid int64 and parses as
// UnaryOp(-, Constant "9223372036854775808"), so the lexeme's magnitude is
// exactly 2^63. This is therefore a magnitude predicate, and WHOEVER HOLDS
// THE SIGN APPLIES IT -- Spec 5b's expression typer, looking at
// UnaryOp(-, Constant). Accepting 2^63 is the loosest bound that never flags
// a literal that could legitimately be compiled.
//
// `allow_two_to_63` is the one knob the sign-holder needs: ExpressionTyper
// passes true for a Constant it knows is the operand of a UnaryOp(-) (where
// 2^63 itself is representable, as -2^63), and false everywhere else (where
// a bare `9223372036854775808` is one past int64_t's positive range and must
// be flagged). This is still not a decode -- the digit walk below only ever
// compares against a fixed limit, the same way it always has; it never
// produces a value a caller could read back out.
//
// Reports nothing, so its tests need no sink. The caller reports, and the
// code is OverflowError rather than TypeError: mypy --strict ACCEPTS
// arbitrarily large integer literals, so a TypeError here would break the
// spec's invariant that a program mypy accepts draws no TypeError. This is a
// capability claim -- the target cannot represent it -- and OverflowError is
// exactly what CPython raises converting an integer too large for a C integer.
//
// Underscore separators are ignored and the 0x/0o/0b prefixes are read. A
// lexeme carrying anything else is accepted: it is not an integer literal
// this function can validate, and a false report is worse than no report.
bool integer_literal_fits_64_bits(const std::string& lexeme, bool allow_two_to_63 = true);

} // namespace cythonpp::domain::semantic

#endif // CYTHONPP_DOMAIN_SEMANTIC_LITERAL_TYPE_H
