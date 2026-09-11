#include <gtest/gtest.h>

#include <string>

#include "domain/diagnostics/diagnostic_sink.h"

namespace cythonpp::domain::diagnostics {
namespace {

TEST(DiagnosticSink, StartsEmpty) {
    DiagnosticSink sink;
    EXPECT_TRUE(sink.empty());
    EXPECT_FALSE(sink.has_errors());
    EXPECT_TRUE(sink.diagnostics().empty());
}

TEST(DiagnosticSink, RecordsSeverityCodeMessageAndPosition) {
    DiagnosticSink sink;
    sink.report_error("TabError", "inconsistent use of tabs and spaces in indentation", 7, 3,
                      Suppressibility::NotSuppressible);

    ASSERT_EQ(sink.diagnostics().size(), 1u);
    const Diagnostic& diagnostic = sink.diagnostics().front();
    EXPECT_EQ(diagnostic.severity, Severity::Error);
    EXPECT_EQ(diagnostic.code, "TabError");
    EXPECT_EQ(diagnostic.message, "inconsistent use of tabs and spaces in indentation");
    EXPECT_EQ(diagnostic.line, 7);
    EXPECT_EQ(diagnostic.column, 3);
}

TEST(DiagnosticSink, PreservesReportOrder) {
    DiagnosticSink sink;
    sink.report_error("First", "first", 1, 1, Suppressibility::NotSuppressible);
    sink.report_error("Second", "second", 2, 1, Suppressibility::NotSuppressible);
    sink.report_error("Third", "third", 3, 1, Suppressibility::NotSuppressible);

    ASSERT_EQ(sink.diagnostics().size(), 3u);
    EXPECT_EQ(sink.diagnostics()[0].code, "First");
    EXPECT_EQ(sink.diagnostics()[1].code, "Second");
    EXPECT_EQ(sink.diagnostics()[2].code, "Third");
}

TEST(DiagnosticSink, WarningsDoNotCountAsErrors) {
    DiagnosticSink sink;
    sink.report(Diagnostic{Severity::Warning, "Advice", "consider not doing that", 1, 1},
                Suppressibility::NotSuppressible);

    EXPECT_FALSE(sink.empty());
    EXPECT_FALSE(sink.has_errors());
}

// A Suppressible diagnostic is DROPPED, not merely hidden: it never reaches
// diagnostics(), so empty()/has_errors() cannot see it either. A
// NotSuppressible one reported in the same region is untouched -- which is
// what lets the semantic pass silence its type-checker judgements in
// unreachable code while keeping everything mypy's semantic analyzer would
// still report there.
//
// THE AXIS IS THE DIAGNOSTIC'S OWN, NOT ITS CODE. Both diagnostics below
// carry the SAME code, deliberately: cythonpp spells "TypeError" for
// judgements on both sides of that line, so a filter keyed on the code string
// cannot express this and a filter keyed on the code string is exactly what
// what an earlier version of this filter used, and it silently accepted nine
// measured classes of rejected program.
TEST(DiagnosticSink, SuppressesOnlyTheSuppressibleDiagnosticEvenUnderOneCode) {
    DiagnosticSink sink;
    {
        DiagnosticSuppression suppression(sink);
        sink.report_error("TypeError", "dropped", 1, 1, Suppressibility::Suppressible);
        sink.report_error("TypeError", "kept", 2, 1, Suppressibility::NotSuppressible);
    }

    ASSERT_EQ(sink.diagnostics().size(), 1u);
    EXPECT_EQ(sink.diagnostics()[0].code, "TypeError");
    EXPECT_EQ(sink.diagnostics()[0].message, "kept");
    EXPECT_EQ(sink.diagnostics()[0].line, 2);
    EXPECT_TRUE(sink.has_errors());
}

// The guard's scope is the region, and reporting resumes exactly when it ends.
TEST(DiagnosticSink, ReportingResumesWhenTheGuardIsDestroyed) {
    DiagnosticSink sink;
    { DiagnosticSuppression suppression(sink); }
    sink.report_error("TypeError", "kept", 1, 1, Suppressibility::Suppressible);

    ASSERT_EQ(sink.diagnostics().size(), 1u);
    EXPECT_EQ(sink.diagnostics()[0].message, "kept");
    EXPECT_EQ(sink.diagnostics()[0].line, 1);
}

// THE REASON IT IS A DEPTH COUNTER AND NOT A BOOL. Unreachable regions nest,
// so a suppression is pushed twice; when the inner region ends the outer one
// is still live and must still suppress. A bool would un-suppress here.
TEST(DiagnosticSink, NestedSuppressionsDoNotUnsuppressEachOther) {
    DiagnosticSink sink;
    {
        DiagnosticSuppression outer(sink);
        { DiagnosticSuppression inner(sink); }
        sink.report_error("TypeError", "still dropped", 1, 1, Suppressibility::Suppressible);
    }
    sink.report_error("TypeError", "kept", 2, 1, Suppressibility::Suppressible);

    ASSERT_EQ(sink.diagnostics().size(), 1u);
    EXPECT_EQ(sink.diagnostics()[0].message, "kept");
    EXPECT_EQ(sink.diagnostics()[0].line, 2);
}

} // namespace
} // namespace cythonpp::domain::diagnostics
