#ifndef CYTHONPP_ADAPTERS_FILESYSTEM_FILESYSTEM_OUTPUT_WRITER_H
#define CYTHONPP_ADAPTERS_FILESYSTEM_FILESYSTEM_OUTPUT_WRITER_H

#include <string>

#include "ports/output_writer.h"

namespace cythonpp::adapters::filesystem {

// Concrete OutputWriter that writes generated code to the local filesystem.
// Wired in by CliAdapter's --emit-cpp flag, which is ports::OutputWriter's
// first and only consumer.
class FilesystemOutputWriter : public ports::OutputWriter {
public:
    void write(const std::string& path, const std::string& contents) override;
};

} // namespace cythonpp::adapters::filesystem

#endif // CYTHONPP_ADAPTERS_FILESYSTEM_FILESYSTEM_OUTPUT_WRITER_H
