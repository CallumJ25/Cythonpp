#include "domain/semantic/rule_result.h"

#include <string>
#include <utility>

namespace cythonpp::domain::semantic {

std::string unsupported_message(UnsupportedReason reason) {
    switch (reason) {
        case UnsupportedReason::UnionOperand:
            return "operations on a union-typed value require narrowing, which is not supported";
        case UnsupportedReason::UserClassOperator:
            return "operators on user-defined class instances are not supported";
        case UnsupportedReason::UserClassIteration:
            return "iterating an instance of a user-defined class is not supported";
        case UnsupportedReason::TupleRepeat:
            return "repeating a tuple by an integer is not supported";
    }
    // Unreachable for a valid enumerator, and there is deliberately no
    // default: arm above so adding a reason is a compile error here.
    return "not supported";
}

RuleResult RuleResult::ok(Type type) {
    RuleResult result;
    result.status = Status::Ok;
    result.type = std::move(type);
    return result;
}

RuleResult RuleResult::not_applicable() { return RuleResult(); }

RuleResult RuleResult::unsupported(UnsupportedReason reason) {
    RuleResult result;
    result.status = Status::Unsupported;
    result.reason = reason;
    return result;
}

} // namespace cythonpp::domain::semantic
