#include "domain/diagnostics/diagnostic_sink.h"

#include <algorithm>
#include <utility>

namespace cythonpp::domain::diagnostics {

void DiagnosticSink::report(Diagnostic diagnostic) { diagnostics_.push_back(std::move(diagnostic)); }

void DiagnosticSink::report_error(std::string code, std::string message, int line, int column) {
    report(Diagnostic{Severity::Error, std::move(code), std::move(message), line, column});
}

const std::vector<Diagnostic>& DiagnosticSink::diagnostics() const { return diagnostics_; }

bool DiagnosticSink::has_errors() const {
    return std::any_of(diagnostics_.begin(), diagnostics_.end(),
                       [](const Diagnostic& diagnostic) { return diagnostic.severity == Severity::Error; });
}

bool DiagnosticSink::empty() const { return diagnostics_.empty(); }

} // namespace cythonpp::domain::diagnostics
