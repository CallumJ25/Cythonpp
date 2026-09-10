#include "domain/diagnostics/diagnostic_sink.h"

#include <algorithm>
#include <utility>

namespace cythonpp::domain::diagnostics {

void DiagnosticSink::report(Diagnostic diagnostic) {
    if (is_suppressed(diagnostic.code)) {
        return;
    }
    diagnostics_.push_back(std::move(diagnostic));
}

void DiagnosticSink::report_error(std::string code, std::string message, int line, int column) {
    report(Diagnostic{Severity::Error, std::move(code), std::move(message), line, column});
}

const std::vector<Diagnostic>& DiagnosticSink::diagnostics() const { return diagnostics_; }

bool DiagnosticSink::has_errors() const {
    return std::any_of(diagnostics_.begin(), diagnostics_.end(),
                       [](const Diagnostic& diagnostic) { return diagnostic.severity == Severity::Error; });
}

bool DiagnosticSink::empty() const { return diagnostics_.empty(); }

void DiagnosticSink::push_suppressed_code(std::string code) {
    suppressed_codes_.push_back(std::move(code));
}

void DiagnosticSink::pop_suppressed_code() {
    if (!suppressed_codes_.empty()) {
        suppressed_codes_.pop_back();
    }
}

bool DiagnosticSink::is_suppressed(const std::string& code) const {
    return std::find(suppressed_codes_.begin(), suppressed_codes_.end(), code) !=
           suppressed_codes_.end();
}

} // namespace cythonpp::domain::diagnostics
