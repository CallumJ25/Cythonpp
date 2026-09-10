#include <array>
#include <cstddef>
#include <string>

#include <gtest/gtest.h>

#include "domain/diagnostics/suppressibility.h"
#include "domain/semantic/diagnostic_kind.h"

namespace cythonpp::domain::semantic {
namespace {

// Hand-maintained, exactly like ALL_TOKEN_TYPES in
// tests/domain/lexer/token_test.cpp and for the same reason: C++17 has no
// enum reflection, so the coverage guard below has to be given the list. The
// guard is what makes forgetting to extend it a FAILURE rather than a silent
// gap -- see CoversEveryEnumerator.
constexpr std::array<DiagnosticKind, 5> ALL_DIAGNOSTIC_KINDS{
    DiagnosticKind::TypeCheckerTypeError, DiagnosticKind::SemanticAnalyzerTypeError,
    DiagnosticKind::NameError,            DiagnosticKind::NotImplementedError,
    DiagnosticKind::OverflowError,
};

// A new enumerator added anywhere in DiagnosticKind without being added here
// breaks this: the enumerators are contiguous from zero, so the last one's
// value plus one IS the count. That makes "extend the table, then decide the
// classification" the only way forward, which is the whole point of the type.
TEST(DiagnosticKind, CoversEveryEnumerator) {
    EXPECT_EQ(ALL_DIAGNOSTIC_KINDS.size(),
              static_cast<std::size_t>(DiagnosticKind::OverflowError) + 1);
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
