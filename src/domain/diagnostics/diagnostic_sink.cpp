#include "domain/diagnostics/diagnostic_sink.h"

#include <algorithm>
#include <utility>

namespace cythonpp::domain::diagnostics {

void DiagnosticSink::report(Diagnostic diagnostic, Suppressibility suppressibility) {
    if (suppressibility == Suppressibility::Suppressible && suppression_depth_ > 0) {
        return;
    }
    diagnostics_.push_back(std::move(diagnostic));
}

void DiagnosticSink::report_error(std::string code, std::string message, int line, int column,
                                  Suppressibility suppressibility) {
    report(Diagnostic{Severity::Error, std::move(code), std::move(message), line, column},
           suppressibility);
}

const std::vector<Diagnostic>& DiagnosticSink::diagnostics() const { return diagnostics_; }

bool DiagnosticSink::has_errors() const {
    return std::any_of(diagnostics_.begin(), diagnostics_.end(),
                       [](const Diagnostic& diagnostic) { return diagnostic.severity == Severity::Error; });
}

bool DiagnosticSink::empty() const { return diagnostics_.empty(); }

void DiagnosticSink::push_suppression() { ++suppression_depth_; }

void DiagnosticSink::pop_suppression() {
    if (suppression_depth_ > 0) {
        --suppression_depth_;
    }
}

} // namespace cythonpp::domain::diagnostics
