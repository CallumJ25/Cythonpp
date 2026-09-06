#ifndef CYTHONPP_DOMAIN_PARSER_ASSIGNABILITY_H
#define CYTHONPP_DOMAIN_PARSER_ASSIGNABILITY_H

#include <string>

#include "domain/ast/expr.h"

namespace cythonpp::domain::parser {

// Whether `expr` can appear on the left of an assignment: a Name, an
// Attribute, a Subscript, or a non-empty TupleExpr of those, recursively.
//
// Shared rather than private to either parser: ExpressionParser needs it for
// `for` and comprehension targets, StatementParser needs it for assignment
// targets, and two copies would drift into two different answers.
bool is_assignable(const ast::Expr& expr);

// Why `expr` cannot be assigned to, in CPython's phrasing family so the
// message reads like one a user has seen before. Only meaningful when
// is_assignable(expr) is false.
std::string not_assignable_message(const ast::Expr& expr);

} // namespace cythonpp::domain::parser

#endif // CYTHONPP_DOMAIN_PARSER_ASSIGNABILITY_H
