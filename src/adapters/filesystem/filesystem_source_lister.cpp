#include "adapters/filesystem/filesystem_source_lister.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <stdexcept>
#include <system_error>

namespace cythonpp::adapters::filesystem {

// An alias, not a using-directive: this namespace is itself named
// `filesystem`, so an unqualified `filesystem::path` here would resolve to
// cythonpp::adapters::filesystem rather than std.
namespace fs = std::filesystem;

namespace {

std::string to_lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return text;
}

} // namespace

bool FilesystemSourceLister::is_python_source(const std::string& filename) {
    // Case-insensitive because Windows filesystems are, so a file created as
    // FOO.PY must not be silently dropped. Note .pyi stubs are excluded:
    // they carry type information, not compilable source.
    const std::string lowered = to_lower(filename);
    return lowered.size() > 3 && lowered.compare(lowered.size() - 3, 3, ".py") == 0;
}

bool FilesystemSourceLister::is_skipped_directory(const std::string& directory_name) {
    // A leading dot covers .venv, .env, .git, .mypy_cache and anything else
    // tooling drops in a project root. Note this does not catch a non-dotted
    // `venv`, `site-packages` or `build`.
    return directory_name == "__pycache__" || (!directory_name.empty() && directory_name[0] == '.');
}

std::vector<std::string> FilesystemSourceLister::list(const std::string& directory) const {
    std::error_code error;
    fs::recursive_directory_iterator iterator(directory, fs::directory_options::skip_permission_denied, error);
    if (error) {
        throw std::runtime_error("could not list source directory: " + directory);
    }

    std::vector<std::string> paths;
    // An explicit iterator loop rather than range-for: pruning a subtree
    // needs disable_recursion_pending() on the iterator itself, so skipped
    // trees are never descended into instead of being filtered afterwards.
    const fs::recursive_directory_iterator end;
    for (; iterator != end; iterator.increment(error)) {
        if (error) {
            throw std::runtime_error("could not list source directory: " + directory);
        }

        const fs::directory_entry& entry = *iterator;
        const std::string name = entry.path().filename().string();

        if (entry.is_directory()) {
            if (is_skipped_directory(name)) {
                iterator.disable_recursion_pending();
            }
            continue;
        }
        if (entry.is_regular_file() && is_python_source(name)) {
            // generic_string() so the separator is always '/', making the
            // map keys byte-identical across platforms. Paths stay as
            // iterated -- relative if `directory` was -- because they become
            // the result keys and feed straight back into SourceReader.
            paths.push_back(entry.path().generic_string());
        }
    }

    // Directory iteration order is unspecified and differs by filesystem, so
    // sort to keep console output and test expectations deterministic.
    std::sort(paths.begin(), paths.end());
    return paths;
}

} // namespace cythonpp::adapters::filesystem
