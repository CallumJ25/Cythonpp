#ifndef CYTHONPP_ADAPTERS_FILESYSTEM_FILESYSTEM_SOURCE_READER_H
#define CYTHONPP_ADAPTERS_FILESYSTEM_FILESYSTEM_SOURCE_READER_H

#include <string>

#include "ports/source_reader.h"

namespace cythonpp::adapters::filesystem {

// Concrete SourceReader that reads source text off the local filesystem.
class FilesystemSourceReader : public ports::SourceReader {
public:
    // Throws std::runtime_error if the file cannot be opened.
    std::string read(const std::string& path) const override;
};

} // namespace cythonpp::adapters::filesystem

#endif // CYTHONPP_ADAPTERS_FILESYSTEM_FILESYSTEM_SOURCE_READER_H
