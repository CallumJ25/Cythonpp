#include <string>

#include <gtest/gtest.h>

#include "domain/semantic/rule_result.h"
#include "domain/semantic/type.h"

namespace cythonpp::domain::semantic {
namespace {

TEST(RuleResult, OkCarriesItsType) {
    const RuleResult result = RuleResult::ok(Type::int_());

    EXPECT_EQ(result.status, RuleResult::Status::Ok);
    EXPECT_EQ(result.type, Type::int_());
}

TEST(RuleResult, NotApplicableIsTheDefaultConstructedStatus) {
    const RuleResult explicit_result = RuleResult::not_applicable();
    EXPECT_EQ(explicit_result.status, RuleResult::Status::NotApplicable);

    // A value-initialized RuleResult must be the SAFE status: a caller that
    // forgets to assign one gets "report a type error", never a silent Ok
    // carrying a default Type (which would be Unknown, and Unknown is
    // absorbing, so it would silence a genuine error).
    const RuleResult defaulted;
    EXPECT_EQ(defaulted.status, RuleResult::Status::NotApplicable);
}

TEST(RuleResult, UnsupportedCarriesItsReason) {
    const RuleResult result = RuleResult::unsupported(UnsupportedReason::TupleRepeat);

    EXPECT_EQ(result.status, RuleResult::Status::Unsupported);
    EXPECT_EQ(result.reason, UnsupportedReason::TupleRepeat);
}

// The messages are asserted verbatim because the corpus harness (Task 25)
// matches them character for character, and because a reworded deferral
// message is a silent corpus break.
TEST(UnsupportedMessage, RendersEveryReason) {
    EXPECT_EQ(unsupported_message(UnsupportedReason::UnionOperand),
              "operations on a union-typed value require narrowing, which is not supported");
    EXPECT_EQ(unsupported_message(UnsupportedReason::UserClassOperator),
              "operators on user-defined class instances are not supported");
    EXPECT_EQ(unsupported_message(UnsupportedReason::TupleRepeat),
              "repeating a tuple by an integer is not supported");
}

} // namespace
} // namespace cythonpp::domain::semantic
