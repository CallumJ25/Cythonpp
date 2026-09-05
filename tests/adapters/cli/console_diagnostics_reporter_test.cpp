#include <gtest/gtest.h>

#include <iostream>
#include <sstream>
#include <streambuf>
#include <string>

#include "adapters/cli/console_diagnostics_reporter.h"

namespace cythonpp::adapters::cli {
namespace {

// Swaps std::cerr's buffer for the lifetime of the object, so a test can read
// back what the reporter wrote without the output polluting the test run.
class CapturedCerr {
public:
    CapturedCerr() : original_(std::cerr.rdbuf(captured_.rdbuf())) {}
    ~CapturedCerr() { std::cerr.rdbuf(original_); }

    std::string str() const { return captured_.str(); }

private:
    std::ostringstream captured_;
    std::streambuf* original_;
};

TEST(ConsoleDiagnosticsReporter, WritesPathLineColumnSeverityCodeAndMessage) {
    CapturedCerr captured;
    ConsoleDiagnosticsReporter reporter;
    reporter.report("pkg/a.py", domain::diagnostics::Diagnostic{domain::diagnostics::Severity::Error,
                                                                "TabError", "bad indentation", 7, 3});

    EXPECT_EQ(captured.str(), "pkg/a.py:7:3: error: TabError: bad indentation\n");
}

TEST(ConsoleDiagnosticsReporter, LabelsWarningsAsWarnings) {
    CapturedCerr captured;
    ConsoleDiagnosticsReporter reporter;
    reporter.report("a.py", domain::diagnostics::Diagnostic{domain::diagnostics::Severity::Warning,
                                                            "Advice", "reconsider", 1, 1});

    EXPECT_EQ(captured.str(), "a.py:1:1: warning: Advice: reconsider\n");
}

} // namespace
} // namespace cythonpp::adapters::cli
