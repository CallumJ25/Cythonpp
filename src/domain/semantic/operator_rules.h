#ifndef CYTHONPP_DOMAIN_SEMANTIC_OPERATOR_RULES_H
#define CYTHONPP_DOMAIN_SEMANTIC_OPERATOR_RULES_H

#include <vector>

#include "class_lookup.h"
#include "domain/lexer/token_type.h"
#include "rule_result.h"
#include "type.h"

namespace cythonpp::domain::semantic {

// What Python's operators do to types, in one place -- the spirit of
// precedence_table.cpp: tabular, and independently testable.
//
// Every function here is a PURE FUNCTION of its inputs and takes no
// DiagnosticSink. It answers three ways, and the caller must handle all
// three (see RuleResult):
//
//   Ok            -- use the type.
//   NotApplicable -- a genuine type error; the caller reports TypeError.
//   Unsupported   -- this compiler cannot model the answer; the caller
//                    reports NotImplementedError with
//                    unsupported_message(reason), NEVER TypeError, because
//                    the program may be one mypy --strict accepts.
//
// The three-valued answer replaced a std::optional<Type> whose nullopt
// conflated the last two, which made a false TypeError reachable by
// omission at every call site.
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
RuleResult binary_result(lexer::token_type op, const Type& left, const Type& right);

// `op operand`, for `-`, `+`, `~` and `not`.
RuleResult unary_result(lexer::token_type op, const Type& operand);

// One comparison from an ast::Compare chain. Always Bool when it applies.
RuleResult comparison_result(lexer::token_type op, const Type& left, const Type& right);

// `container[index]`.
//
// `classes` is forwarded to is_subtype for the dict key check, so a dict
// keyed by a base class accepts a subclass index. It may be null, in which
// case two differently-named classes are simply unrelated.
RuleResult subscript_result(const Type& container, const Type& index,
                            const ClassLookup* classes = nullptr);

// What iterating `iterable` yields, for `for` and for comprehensions.
// Iterating a dict yields its KEYS, not its items.
RuleResult element_type(const Type& iterable);

// `a and b`, `a or b` -- one ast::BoolOp, whose `values()` may hold more than
// two operands because the parser flattens a chain.
//
// Unknown is absorbing here, like binary_result. A Union operand is
// Unsupported(UnionOperand): mypy NARROWS, so `(int | None) or 0` is int, and
// producing int | None instead would make `y: int = x or 0` a false
// TypeError.
//
// Non-union operands yield union_of(operands). mypy's answer applies
// falsiness narrowing -- `int and str` is Literal[0] | str -- and int | str is
// a supertype of that, so the widening only ever misses an error.
RuleResult boolop_result(lexer::token_type op, const std::vector<Type>& operands);

} // namespace cythonpp::domain::semantic

#endif // CYTHONPP_DOMAIN_SEMANTIC_OPERATOR_RULES_H
