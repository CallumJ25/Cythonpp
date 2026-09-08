#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <streambuf>
#include <string>
#include <vector>

#include "adapters/cli/cli_adapter.h"

namespace cythonpp::adapters::cli {
namespace {

// Swaps both std::cout and std::cerr's buffers for the lifetime of the
// object, mirroring ConsoleDiagnosticsReporterTest's CapturedCerr. CliAdapter
// prints straight to the streams -- it is the composition root, not a port
// with an injectable sink -- so capturing the real streams is the only way
// to observe its output.
class CapturedStreams {
public:
    CapturedStreams()
        : original_cout_(std::cout.rdbuf(captured_cout_.rdbuf())),
          original_cerr_(std::cerr.rdbuf(captured_cerr_.rdbuf())) {}
    ~CapturedStreams() {
        std::cout.rdbuf(original_cout_);
        std::cerr.rdbuf(original_cerr_);
    }

    std::string out() const { return captured_cout_.str(); }
    std::string err() const { return captured_cerr_.str(); }

private:
    std::ostringstream captured_cout_;
    std::ostringstream captured_cerr_;
    std::streambuf* original_cout_;
    std::streambuf* original_cerr_;
};

// Writes `contents` to a uniquely-named file under the system temp directory
// and removes it on destruction. CliAdapter builds its own concrete
// FilesystemSourceReader internally rather than accepting an injected port
// (it is the composition root), so exercising it end to end needs a real
// file on disk.
class TempPythonFile {
public:
    explicit TempPythonFile(const std::string& contents) {
        path_ = std::filesystem::temp_directory_path() /
                ("cythonpp_cli_adapter_test_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)) + ".py");
        std::ofstream file(path_);
        file << contents;
    }
    ~TempPythonFile() { std::filesystem::remove(path_); }

    std::string path() const { return path_.string(); }

private:
    std::filesystem::path path_;
};

// Runs CliAdapter with the given argv-style arguments (argv[0] is synthesized)
// and returns its exit code, with stdout/stderr captured into `out`/`err`.
int run_cli(const std::vector<std::string>& args, std::string& out, std::string& err) {
    std::vector<std::string> argv_storage = {"cythonpp"};
    argv_storage.insert(argv_storage.end(), args.begin(), args.end());

    std::vector<char*> argv;
    for (std::string& arg : argv_storage) {
        argv.push_back(arg.data());
    }

    CapturedStreams captured;
    CliAdapter adapter;
    const int exit_code = adapter.run(static_cast<int>(argv.size()), argv.data());
    out = captured.out();
    err = captured.err();
    return exit_code;
}

TEST(CliAdapter, DefaultModePrintsPlainTreeWithoutTypeSuffixesAndExitsZero) {
    TempPythonFile file("x: int = 5\n");
    std::string out;
    std::string err;

    const int exit_code = run_cli({file.path()}, out, err);

    EXPECT_EQ(exit_code, 0);
    EXPECT_NE(out.find("(AnnAssign"), std::string::npos);
    EXPECT_EQ(out.find(":int"), std::string::npos);
    EXPECT_EQ(err, "");
}

TEST(CliAdapter, TokensFlagPrintsTokenDumpAndExitsZero) {
    TempPythonFile file("x: int = 5\n");
    std::string out;
    std::string err;

    const int exit_code = run_cli({"--tokens", file.path()}, out, err);

    EXPECT_EQ(exit_code, 0);
    EXPECT_NE(out.find("tokens) ==="), std::string::npos);
    EXPECT_NE(out.find("IDENTIFIER"), std::string::npos);
    EXPECT_EQ(err, "");
}

TEST(CliAdapter, TypesFlagPrintsTypedTreeAndExitsZeroForAWellTypedFile) {
    TempPythonFile file("x: int = 5\n");
    std::string out;
    std::string err;

    const int exit_code = run_cli({"--types", file.path()}, out, err);

    EXPECT_EQ(exit_code, 0);
    EXPECT_NE(out.find("(AnnAssign"), std::string::npos);
    EXPECT_NE(out.find(":int"), std::string::npos);
    EXPECT_EQ(err, "");
}

TEST(CliAdapter, TypesFlagStillPrintsTheTypedTreeAndReportsErrorsForABadlyTypedFile) {
    TempPythonFile file("x: int = \"s\"\n");
    std::string out;
    std::string err;

    const int exit_code = run_cli({"--types", file.path()}, out, err);

    EXPECT_EQ(exit_code, 1);
    EXPECT_NE(err.find("TypeError"), std::string::npos);
    // The tree is still printed for a file with errors -- seeing the output
    // is exactly what helps when diagnosing one.
    EXPECT_NE(out.find("(AnnAssign"), std::string::npos);
}

} // namespace
} // namespace cythonpp::adapters::cli
