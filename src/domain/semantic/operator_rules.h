#ifndef CYTHONPP_DOMAIN_SEMANTIC_OPERATOR_RULES_H
#define CYTHONPP_DOMAIN_SEMANTIC_OPERATOR_RULES_H

#include <optional>

#include "class_lookup.h"
#include "domain/lexer/token_type.h"
#include "type.h"

namespace cythonpp::domain::semantic {

// What Python's operators do to types, in one place -- the spirit of
// precedence_table.cpp: tabular, and independently testable.
//
// Every function here is a PURE FUNCTION of its inputs and takes no
// DiagnosticSink. std::nullopt means "this operator does not apply", which is
// the caller's cue to report; the table never reports, so its tests need no
// sink.
//
// HOW Unknown PROPAGATES, stated per function because one blanket rule would
// be wrong for two of them:
//
//   binary_result, subscript_result, element_type -- return Unknown when any
//   input is Unknown, never nullopt, so a root cause reports once and its
//   uses stay silent.
//
//   unary_result -- the same, EXCEPT for `not`.
//
//   comparison_result -- never returns Unknown at all.
//
// `not` and every comparison always yield Bool, because Python's truthiness
// and identity comparisons are total and their result type does not depend on
// the operand's type. An implementation that absorbed Unknown into a `not` or
// an `==` would turn Bool into Unknown and silence a genuine error downstream.

// `left op right`, for the arithmetic and bitwise operators ast::BinOp
// carries. Comparison and boolean operators are not here: they build
// ast::Compare and ast::BoolOp, not ast::BinOp.
std::optional<Type> binary_result(lexer::token_type op, const Type& left, const Type& right);

// `op operand`, for `-`, `+`, `~` and `not`.
std::optional<Type> unary_result(lexer::token_type op, const Type& operand);

// One comparison from an ast::Compare chain. Always Bool when it applies.
std::optional<Type> comparison_result(lexer::token_type op, const Type& left, const Type& right);

// `container[index]`.
//
// `classes` is forwarded to is_subtype for the dict key check, so a dict
// keyed by a base class accepts a subclass index. It may be null, in which
// case two differently-named classes are simply unrelated.
std::optional<Type> subscript_result(const Type& container, const Type& index,
                                     const ClassLookup* classes = nullptr);

// What iterating `iterable` yields, for `for` and for comprehensions.
// Iterating a dict yields its KEYS, not its items.
std::optional<Type> element_type(const Type& iterable);

} // namespace cythonpp::domain::semantic

#endif // CYTHONPP_DOMAIN_SEMANTIC_OPERATOR_RULES_H
