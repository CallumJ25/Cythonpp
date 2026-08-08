#include "adapters/filesystem/filesystem_source_reader.h"

#include <fstream>
#include <sstream>
#include <stdexcept>

namespace cythonpp::adapters::filesystem {

std::string FilesystemSourceReader::read(const std::string& path) const {
    std::ifstream file(path);
    if (!file) {
        throw std::runtime_error("could not open source file: " + path);
    }

    std::ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

} // namespace cythonpp::adapters::filesystem
