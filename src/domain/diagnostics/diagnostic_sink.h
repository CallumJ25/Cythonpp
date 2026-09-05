#ifndef CYTHONPP_DOMAIN_DIAGNOSTICS_DIAGNOSTIC_SINK_H
#define CYTHONPP_DOMAIN_DIAGNOSTICS_DIAGNOSTIC_SINK_H

#include <string>
#include <vector>

#include "diagnostic.h"

namespace cythonpp::domain::diagnostics {

// Collects the diagnostics a pass produces, in report order.
//
// A concrete class rather than an interface: domain code must not depend on
// ports/, and a pass has no business knowing whether its output is printed,
// buffered, or discarded. The application layer drains a sink into
// ports::DiagnosticsReporter, which is where that choice belongs.
class DiagnosticSink {
public:
    void report(Diagnostic diagnostic);

    // The common case, spelled so call sites stay off aggregate initialization
    // -- adding a field to Diagnostic should not break every reporting pass.
    void report_error(std::string code, std::string message, int line, int column);

    const std::vector<Diagnostic>& diagnostics() const;

    // True if any diagnostic has Severity::Error. Distinct from !empty(),
    // which a warning also satisfies.
    bool has_errors() const;
    bool empty() const;

private:
    std::vector<Diagnostic> diagnostics_;
};

} // namespace cythonpp::domain::diagnostics

#endif // CYTHONPP_DOMAIN_DIAGNOSTICS_DIAGNOSTIC_SINK_H
