#include "adapters/filesystem/filesystem_output_writer.h"

#include <fstream>
#include <stdexcept>

namespace cythonpp::adapters::filesystem {

void FilesystemOutputWriter::write(const std::string& path, const std::string& contents) {
    std::ofstream file(path);
    if (!file) {
        throw std::runtime_error("could not open output file: " + path);
    }
    file << contents;
}

} // namespace cythonpp::adapters::filesystem
