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
    sink.report_error("TabError", "inconsistent use of tabs and spaces in indentation", 7, 3);

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
    sink.report_error("First", "first", 1, 1);
    sink.report_error("Second", "second", 2, 1);
    sink.report_error("Third", "third", 3, 1);

    ASSERT_EQ(sink.diagnostics().size(), 3u);
    EXPECT_EQ(sink.diagnostics()[0].code, "First");
    EXPECT_EQ(sink.diagnostics()[1].code, "Second");
    EXPECT_EQ(sink.diagnostics()[2].code, "Third");
}

TEST(DiagnosticSink, WarningsDoNotCountAsErrors) {
    DiagnosticSink sink;
    sink.report(Diagnostic{Severity::Warning, "Advice", "consider not doing that", 1, 1});

    EXPECT_FALSE(sink.empty());
    EXPECT_FALSE(sink.has_errors());
}

} // namespace
} // namespace cythonpp::domain::diagnostics
