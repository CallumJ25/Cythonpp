#ifndef CYTHONPP_ADAPTERS_CLI_CONSOLE_DIAGNOSTICS_REPORTER_H
#define CYTHONPP_ADAPTERS_CLI_CONSOLE_DIAGNOSTICS_REPORTER_H

#include <string>

#include "ports/diagnostics_reporter.h"

namespace cythonpp::adapters::cli {

// Prints diagnostics to stderr in the shape clang and gcc use, so editors and
// build-tool problem matchers that already parse compiler output can jump
// straight to the offending line without configuration.
//
// stderr rather than stdout because stdout carries the token dump, and a user
// piping that to a file still wants to see the errors.
class ConsoleDiagnosticsReporter : public ports::DiagnosticsReporter {
public:
    void report(const std::string& path, const domain::diagnostics::Diagnostic& diagnostic) override;
};

} // namespace cythonpp::adapters::cli

#endif // CYTHONPP_ADAPTERS_CLI_CONSOLE_DIAGNOSTICS_REPORTER_H
