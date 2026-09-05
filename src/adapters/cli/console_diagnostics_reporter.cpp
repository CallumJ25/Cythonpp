#include "adapters/cli/console_diagnostics_reporter.h"

#include <iostream>

namespace cythonpp::adapters::cli {

void ConsoleDiagnosticsReporter::report(const std::string& path,
                                        const domain::diagnostics::Diagnostic& diagnostic) {
    const bool is_error = diagnostic.severity == domain::diagnostics::Severity::Error;
    std::cerr << path << ':' << diagnostic.line << ':' << diagnostic.column << ": "
              << (is_error ? "error" : "warning") << ": " << diagnostic.code << ": "
              << diagnostic.message << std::endl;
}

} // namespace cythonpp::adapters::cli
