#ifndef CYTHONPP_PORTS_OUTPUT_WRITER_H
#define CYTHONPP_PORTS_OUTPUT_WRITER_H

#include <string>

namespace cythonpp::ports {

// Driven port: something the application layer needs in order to emit
// generated C++ output. Concrete adapters (e.g. filesystem) implement this.
class OutputWriter {
public:
    virtual ~OutputWriter() = default;

    virtual void write(const std::string& path, const std::string& contents) = 0;
};

} // namespace cythonpp::ports

#endif // CYTHONPP_PORTS_OUTPUT_WRITER_H
