#ifndef CYTHONPP_PORTS_DIAGNOSTICS_REPORTER_H
#define CYTHONPP_PORTS_DIAGNOSTICS_REPORTER_H

#include <string>

#include "domain/diagnostics/diagnostic.h"

namespace cythonpp::ports {

// Driven port: how the application layer reports problems back out.
//
// This is the one place ports/ depends on domain/. That is ordinary for a
// driven port -- it exists to speak the domain's language to the outside
// world -- and the alternative is flattening Diagnostic back into loose
// parameters at every call site, losing severity and code on the way.
class DiagnosticsReporter {
public:
    virtual ~DiagnosticsReporter() = default;

    // `path` is the source file the problem was found in. Diagnostic does not
    // carry it: a domain pass only ever sees one file's tokens and cannot know
    // its path, so the caller that does attaches it here.
    virtual void report(const std::string& path, const domain::diagnostics::Diagnostic& diagnostic) = 0;
};

} // namespace cythonpp::ports

#endif // CYTHONPP_PORTS_DIAGNOSTICS_REPORTER_H
