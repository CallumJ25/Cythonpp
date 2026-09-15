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

TEST(CliAdapter, EmitCppFlagWritesACppFileNextToASupportedInputAndExitsZero) {
    TempPythonFile file("print(1)\n");
    const std::filesystem::path expected_cpp =
        std::filesystem::path(file.path()).replace_extension(".cpp");
    std::filesystem::remove(expected_cpp);
    std::string out;
    std::string err;

    const int exit_code = run_cli({"--emit-cpp", file.path()}, out, err);

    EXPECT_EQ(exit_code, 0);
    ASSERT_TRUE(std::filesystem::exists(expected_cpp));
    std::ostringstream contents;
    {
        // Scoped so the handle is closed before the removal below --
        // Windows refuses to remove a file that is still open.
        std::ifstream written(expected_cpp);
        contents << written.rdbuf();
    }
    EXPECT_NE(contents.str().find("int main()"), std::string::npos);

    std::filesystem::remove(expected_cpp);
}

TEST(CliAdapter, EmitCppFlagWritesNoFileAndExitsNonZeroForAnUnsupportedInput) {
    // A list literal type-checks clean but is outside the emitter's slice, so
    // this exercises codegen's own refusal rather than the type checker's.
    TempPythonFile file("xs: list[int] = []\n");
    const std::filesystem::path expected_cpp =
        std::filesystem::path(file.path()).replace_extension(".cpp");
    std::filesystem::remove(expected_cpp);
    std::string out;
    std::string err;

    const int exit_code = run_cli({"--emit-cpp", file.path()}, out, err);

    EXPECT_NE(exit_code, 0);
    EXPECT_NE(err.find("NotImplementedError"), std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(expected_cpp));
}

// THE SEAM this round's fix closes. Reproduces the reviewer's exact
// scenario: emit successfully once, regress the SAME source file into
// something unsupported, re-run, and confirm the prior run's .cpp does not
// survive -- a stale file that would otherwise be indistinguishable from
// current output. Driven through the real CliAdapter path rather than a
// unit-level stub, since the gap was in the write step, not in the pipeline.
TEST(CliAdapter, EmitCppFlagRemovesAStaleCppWhenAPreviouslyEmittableFileRegresses) {
    TempPythonFile file("print(1)\n");
    const std::filesystem::path expected_cpp =
        std::filesystem::path(file.path()).replace_extension(".cpp");
    std::filesystem::remove(expected_cpp);
    std::string out;
    std::string err;

    ASSERT_EQ(run_cli({"--emit-cpp", file.path()}, out, err), 0);
    ASSERT_TRUE(std::filesystem::exists(expected_cpp)) << "precondition: a prior run wrote it";

    // Regress the very same file in place, exactly as the reviewer's
    // reproduction edits stale.py into an unsupported construct.
    {
        std::ofstream regressed(file.path(), std::ios::trunc);
        regressed << "xs: list[int] = []\n";
    }

    const int exit_code = run_cli({"--emit-cpp", file.path()}, out, err);

    EXPECT_NE(exit_code, 0);
    EXPECT_FALSE(std::filesystem::exists(expected_cpp))
        << "a stale .cpp from the prior successful run must not survive a regression";
}

// The safety half of the same fix: a file at the target path that this
// compiler never wrote (no marker) must be left alone even when the source
// at the same stem fails to emit. Without this, "clean up a stale target"
// could turn into "delete whatever happens to be at this path", which is a
// strictly worse defect than the stale-file problem being fixed.
TEST(CliAdapter, EmitCppFlagLeavesAForeignFileAtTheTargetPathUntouchedOnFailure) {
    TempPythonFile file("xs: list[int] = []\n");
    const std::filesystem::path foreign_cpp =
        std::filesystem::path(file.path()).replace_extension(".cpp");
    std::filesystem::remove(foreign_cpp);
    {
        std::ofstream foreign(foreign_cpp);
        foreign << "// hand-written, not cythonpp output\nint main() { return 0; }\n";
    }
    std::string out;
    std::string err;

    const int exit_code = run_cli({"--emit-cpp", file.path()}, out, err);

    EXPECT_NE(exit_code, 0);
    ASSERT_TRUE(std::filesystem::exists(foreign_cpp))
        << "a file this compiler never wrote must never be deleted";
    std::ostringstream contents;
    {
        std::ifstream reopened(foreign_cpp);
        contents << reopened.rdbuf();
    }
    EXPECT_NE(contents.str().find("hand-written"), std::string::npos);

    std::filesystem::remove(foreign_cpp);
}

} // namespace
} // namespace cythonpp::adapters::cli
