#ifndef CYTHONPP_ADAPTERS_FILESYSTEM_FILESYSTEM_SOURCE_LISTER_H
#define CYTHONPP_ADAPTERS_FILESYSTEM_FILESYSTEM_SOURCE_LISTER_H

#include <string>
#include <vector>

#include "ports/source_lister.h"

namespace cythonpp::adapters::filesystem {

// Concrete SourceLister that walks the local filesystem for Python sources.
class FilesystemSourceLister : public ports::SourceLister {
public:
    // Every .py file under `directory`, recursively, sorted, with '/' as the
    // separator. Throws std::runtime_error if `directory` cannot be
    // traversed.
    std::vector<std::string> list(const std::string& directory) const override;

    // Exposed so the filtering rules can be tested without a real directory
    // tree; the recursive walk around them has nothing else worth testing.
    static bool is_python_source(const std::string& filename);
    static bool is_skipped_directory(const std::string& directory_name);
};

} // namespace cythonpp::adapters::filesystem

#endif // CYTHONPP_ADAPTERS_FILESYSTEM_FILESYSTEM_SOURCE_LISTER_H
