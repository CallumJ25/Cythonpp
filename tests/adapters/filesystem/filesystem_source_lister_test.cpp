#include <gtest/gtest.h>

#include "adapters/filesystem/filesystem_source_lister.h"

namespace cythonpp::adapters::filesystem {
namespace {

// Only the filtering predicates are unit-tested. The recursive walk around
// them would need a committed fixture tree and a compiled-in path to it, for
// far less value than these rules carry.

TEST(FilesystemSourceLister, PythonFilesAreRecognizedBySuffix) {
    EXPECT_TRUE(FilesystemSourceLister::is_python_source("main.py"));
    EXPECT_TRUE(FilesystemSourceLister::is_python_source("a.b.py"));
    EXPECT_TRUE(FilesystemSourceLister::is_python_source("_.py"));
}

TEST(FilesystemSourceLister, NonPythonFilesAreRejected) {
    EXPECT_FALSE(FilesystemSourceLister::is_python_source("notes.txt"));
    EXPECT_FALSE(FilesystemSourceLister::is_python_source("main.pyc"));
    // Stubs carry type information, not compilable source.
    EXPECT_FALSE(FilesystemSourceLister::is_python_source("main.pyi"));
    EXPECT_FALSE(FilesystemSourceLister::is_python_source("py"));
    EXPECT_FALSE(FilesystemSourceLister::is_python_source(".py"));
    EXPECT_FALSE(FilesystemSourceLister::is_python_source("main.py.bak"));
    EXPECT_FALSE(FilesystemSourceLister::is_python_source(""));
}

TEST(FilesystemSourceLister, PythonSuffixMatchIsCaseInsensitive) {
    // Windows filesystems are case-insensitive, so a file created as FOO.PY
    // must not be silently dropped.
    EXPECT_TRUE(FilesystemSourceLister::is_python_source("Main.PY"));
    EXPECT_TRUE(FilesystemSourceLister::is_python_source("MAIN.Py"));
}

TEST(FilesystemSourceLister, PycacheDirectoryIsSkipped) {
    EXPECT_TRUE(FilesystemSourceLister::is_skipped_directory("__pycache__"));
}

TEST(FilesystemSourceLister, DotDirectoriesAreSkipped) {
    EXPECT_TRUE(FilesystemSourceLister::is_skipped_directory(".venv"));
    EXPECT_TRUE(FilesystemSourceLister::is_skipped_directory(".env"));
    EXPECT_TRUE(FilesystemSourceLister::is_skipped_directory(".git"));
    EXPECT_TRUE(FilesystemSourceLister::is_skipped_directory(".mypy_cache"));
}

TEST(FilesystemSourceLister, OrdinaryDirectoriesAreNotSkipped) {
    EXPECT_FALSE(FilesystemSourceLister::is_skipped_directory("src"));
    EXPECT_FALSE(FilesystemSourceLister::is_skipped_directory("tests"));
    EXPECT_FALSE(FilesystemSourceLister::is_skipped_directory("__init__"));
    // Documents a known gap: a virtualenv without a leading dot is walked.
    EXPECT_FALSE(FilesystemSourceLister::is_skipped_directory("venv"));
    EXPECT_FALSE(FilesystemSourceLister::is_skipped_directory("site-packages"));
}

} // namespace
} // namespace cythonpp::adapters::filesystem
