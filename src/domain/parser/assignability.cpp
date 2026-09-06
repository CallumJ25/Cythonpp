#include "assignability.h"

#include "domain/ast/attribute.h"
#include "domain/ast/call.h"
#include "domain/ast/constant.h"
#include "domain/ast/name.h"
#include "domain/ast/subscript.h"
#include "domain/ast/tuple_expr.h"

namespace cythonpp::domain::parser {

bool is_assignable(const ast::Expr& expr) {
    if (dynamic_cast<const ast::Name*>(&expr) != nullptr) {
        return true;
    }
    if (dynamic_cast<const ast::Attribute*>(&expr) != nullptr) {
        return true;
    }
    if (dynamic_cast<const ast::Subscript*>(&expr) != nullptr) {
        return true;
    }
    if (const auto* tuple = dynamic_cast<const ast::TupleExpr*>(&expr)) {
        if (tuple->elements().empty()) {
            return false;
        }
        for (const ast::ExprPtr& element : tuple->elements()) {
            if (!is_assignable(*element)) {
                return false;
            }
        }
        return true;
    }
    return false;
}

// CPython's phrasing family, so the message reads like the one a user has
// seen before.
std::string not_assignable_message(const ast::Expr& expr) {
    if (dynamic_cast<const ast::Constant*>(&expr) != nullptr) {
        return "cannot assign to literal";
    }
    if (dynamic_cast<const ast::Call*>(&expr) != nullptr) {
        return "cannot assign to function call";
    }
    return "cannot assign to this expression";
}

} // namespace cythonpp::domain::parser
