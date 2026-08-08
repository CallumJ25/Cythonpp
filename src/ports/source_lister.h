#ifndef CYTHONPP_PORTS_SOURCE_LISTER_H
#define CYTHONPP_PORTS_SOURCE_LISTER_H

#include <string>
#include <vector>

namespace cythonpp::ports {

// Driven port: something the application layer needs in order to discover
// which source files make up a compilation unit. Concrete adapters (e.g.
// filesystem) implement this. Mirrors SourceReader's const contract, so one
// adapter instance serves any number of calls.
class SourceLister {
public:
    virtual ~SourceLister() = default;

    virtual std::vector<std::string> list(const std::string& directory) const = 0;
};

} // namespace cythonpp::ports

#endif // CYTHONPP_PORTS_SOURCE_LISTER_H
