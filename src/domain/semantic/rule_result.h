#ifndef CYTHONPP_DOMAIN_SEMANTIC_RULE_RESULT_H
#define CYTHONPP_DOMAIN_SEMANTIC_RULE_RESULT_H

#include <string>

#include "type.h"

namespace cythonpp::domain::semantic {

// Why a rule cannot answer, when the reason is this compiler's limits rather
// than the program's types.
//
// An enum rather than a free-text string: the reasons are a closed set, and a
// test asserting on an enumerator cannot drift the way a test asserting on
// prose does. The prose lives in exactly one place, unsupported_message.
enum class UnsupportedReason {
    // Narrowing is deferred. `x: int | None` may be stored, passed and
    // assigned, but `x + 1` cannot be typed without per-branch environments.
    // NOT new work: this deferral already existed and travelled through
    // std::nullopt, relying on every caller to special-case union operands
    // before reporting.
    UnionOperand,

    // Dunder dispatch is deferred. `v + 1` where V defines
    // __add__(self, other: int) -> int is mypy-clean, so reporting a
    // TypeError here would be a false positive; `w + 1` on a class with no
    // __add__ is a genuine error, so the answer depends on the class's
    // members. Deferring costs a named diagnostic; guessing costs the
    // invariant.
    UserClassOperator,

    // `tuple[int, str] * 2` is mypy-clean and yields
    // tuple[int, str, int, str] -- mypy UNROLLS the literal count. The result
    // therefore depends on an operand's literal VALUE, which is constant
    // folding, and Spec 2 ruled that out. Note the in-tree comment at
    // star_result claims the result is a variadic tuple[int, ...]; that is
    // true only for a non-literal count, and either way this compiler cannot
    // represent the answer.
    TupleRepeat,
};

// The user-facing sentence for a reason. Reported with code
// "NotImplementedError" -- never "TypeError", because the program may be one
// mypy --strict accepts.
std::string unsupported_message(UnsupportedReason reason);

// What a rule table answers. Three-valued, because the caller must be able to
// tell a genuine type error from a modelling limit, and a two-valued
// std::optional cannot.
//
// The caller's switch is the point of this type: Ok -> use the type,
// NotApplicable -> report TypeError, Unsupported -> report
// NotImplementedError with unsupported_message(reason). A caller cannot reach
// the false-TypeError path by omission.
struct RuleResult {
    enum class Status { Ok, NotApplicable, Unsupported };

    // NotApplicable is the default deliberately. A value-initialized
    // RuleResult must mean "report", not "Ok with a default Type" -- the
    // default Type is Unknown, Unknown is absorbing, and an accidental
    // Ok(Unknown) would silence a genuine error rather than report it.
    Status status = Status::NotApplicable;

    // Meaningful only when status == Ok.
    Type type;

    // Meaningful only when status == Unsupported.
    UnsupportedReason reason = UnsupportedReason::UnionOperand;

    static RuleResult ok(Type type);
    static RuleResult not_applicable();
    static RuleResult unsupported(UnsupportedReason reason);
};

} // namespace cythonpp::domain::semantic

#endif // CYTHONPP_DOMAIN_SEMANTIC_RULE_RESULT_H
