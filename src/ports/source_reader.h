#ifndef CYTHONPP_PORTS_SOURCE_READER_H
#define CYTHONPP_PORTS_SOURCE_READER_H

#include <string>

namespace cythonpp::ports {

// Driven port: something the application layer needs in order to obtain
// source text. Concrete adapters (e.g. filesystem) implement this.
class SourceReader {
public:
    virtual ~SourceReader() = default;

    virtual std::string read(const std::string& path) const = 0;
};

} // namespace cythonpp::ports

#endif // CYTHONPP_PORTS_SOURCE_READER_H
