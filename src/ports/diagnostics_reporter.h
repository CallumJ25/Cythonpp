#ifndef CYTHONPP_PORTS_DIAGNOSTICS_REPORTER_H
#define CYTHONPP_PORTS_DIAGNOSTICS_REPORTER_H

#include <string>

namespace cythonpp::ports {

// Driven port: how the application layer reports errors/warnings back out.
// Not yet consumed anywhere; stubbed for a future diagnostics pass.
class DiagnosticsReporter {
public:
    virtual ~DiagnosticsReporter() = default;

    virtual void report_error(const std::string& message, int line_number, int column_number) = 0;
};

} // namespace cythonpp::ports

#endif // CYTHONPP_PORTS_DIAGNOSTICS_REPORTER_H
