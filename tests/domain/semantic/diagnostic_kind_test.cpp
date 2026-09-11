#include <array>
#include <cstddef>
#include <string>

#include <gtest/gtest.h>

#include "domain/diagnostics/suppressibility.h"
#include "domain/semantic/diagnostic_kind.h"

namespace cythonpp::domain::semantic {
namespace {

// Hand-maintained, exactly like ALL_TOKEN_TYPES in
// tests/domain/lexer/token_test.cpp and for the same reason: C++17 has no enum
// reflection, so the sweeps below have to be given the list.
//
// WHAT FORCES THIS LIST TO STAY COMPLETE IS THE COMPILER, NOT A TEST, and that
// correction is worth recording because the first version of this file got it
// wrong in precisely the way this task keeps getting things wrong. It carried a
// `CoversEveryEnumerator` test asserting
// `ALL_DIAGNOSTIC_KINDS.size() == static_cast<size_t>(OverflowError) + 1`, on
// the reasoning that the enumerators are contiguous from zero. Probed by
// actually adding a sixth enumerator: the test PASSED, because appending after
// OverflowError does not change OverflowError's own value. It was a guard that
// could not fail for the thing it was named for.
//
// The real guard is diagnostic_kind.h's two exhaustive switches plus
// -Werror=switch (CMakeLists.txt): a new enumerator does not compile until
// both its code and its suppressibility are stated. Re-probed with the same
// sixth enumerator, the build now fails with
// "error: enumeration value 'ProbeSixthKind' not handled in switch". The tests
// below are the SECOND line of defence: they pin what the two tables answer,
// and ExactlyOneKindIsSuppressible... additionally fails if a new kind is
// classified suppressible once it is added here.
constexpr std::array<DiagnosticKind, 5> ALL_DIAGNOSTIC_KINDS{
    DiagnosticKind::TypeCheckerTypeError, DiagnosticKind::SemanticAnalyzerTypeError,
    DiagnosticKind::NameError,            DiagnosticKind::NotImplementedError,
    DiagnosticKind::OverflowError,
};

// The list's own integrity: five DISTINCT enumerators, none repeated. A
// duplicate would silently shrink every sweep below by one -- the same class of
// silent hole the array exists to close.
TEST(DiagnosticKind, TheKindListHasNoDuplicates) {
    for (std::size_t i = 0; i < ALL_DIAGNOSTIC_KINDS.size(); ++i) {
        for (std::size_t j = i + 1; j < ALL_DIAGNOSTIC_KINDS.size(); ++j) {
            EXPECT_NE(ALL_DIAGNOSTIC_KINDS[i], ALL_DIAGNOSTIC_KINDS[j])
                << "duplicate at " << i << " and " << j;
        }
    }
}

// The four-code vocabulary is fixed by the project's rule, so every kind must
// land inside it -- a fifth code would leak this compiler's internal
// classification into labelled output and corpus samples.
TEST(DiagnosticKind, EveryKindSpellsOneOfTheFourProjectCodes) {
    for (const DiagnosticKind kind : ALL_DIAGNOSTIC_KINDS) {
        const std::string code = diagnostic_code(kind);
        EXPECT_TRUE(code == "TypeError" || code == "NameError" ||
                    code == "NotImplementedError" || code == "OverflowError")
            << "unexpected code: " << code;
    }
}

// THE DEFECT THIS TYPE EXISTS TO PREVENT, stated as an assertion: two kinds
// share one code string and differ in suppressibility, so the code string
// cannot decide the question and any mechanism keyed on it is wrong by
// construction.
TEST(DiagnosticKind, TheTwoTypeErrorKindsShareACodeAndDisagreeOnSuppression) {
    EXPECT_STREQ(diagnostic_code(DiagnosticKind::TypeCheckerTypeError), "TypeError");
    EXPECT_STREQ(diagnostic_code(DiagnosticKind::SemanticAnalyzerTypeError), "TypeError");

    EXPECT_EQ(suppressibility_of(DiagnosticKind::TypeCheckerTypeError),
              diagnostics::Suppressibility::Suppressible);
    EXPECT_EQ(suppressibility_of(DiagnosticKind::SemanticAnalyzerTypeError),
              diagnostics::Suppressibility::NotSuppressible);
}

// Only mypy's type checker stops running in unreachable code, so only that one
// kind may be dropped there. Written as a sweep over every kind rather than as
// four separate assertions, so a NEW kind defaulting to Suppressible fails
// here instead of going quietly silent inside an unreachable region.
TEST(DiagnosticKind, ExactlyOneKindIsSuppressibleAndItIsTheTypeCheckerOne) {
    std::size_t suppressible = 0;
    for (const DiagnosticKind kind : ALL_DIAGNOSTIC_KINDS) {
        if (suppressibility_of(kind) == diagnostics::Suppressibility::Suppressible) {
            ++suppressible;
            EXPECT_EQ(kind, DiagnosticKind::TypeCheckerTypeError);
        }
    }
    EXPECT_EQ(suppressible, 1u);
}

} // namespace
} // namespace cythonpp::domain::semantic
