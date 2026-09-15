#ifndef CYTHONPP_TESTS_DOMAIN_CODEGEN_COMPILE_AND_RUN_H
#define CYTHONPP_TESTS_DOMAIN_CODEGEN_COMPILE_AND_RUN_H

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <sstream>
#include <string>

#include <gtest/gtest.h>

#include "domain/codegen/emitter.h"
#include "emitter_fixture.h"

namespace cythonpp::domain::codegen {

struct RunResult {
    int exit_code = 0;
    std::string stdout_text;
};

// WINDOWS QUOTING, MEASURED RATHER THAN ASSUMED. std::system() on this
// platform hands its argument to `cmd.exe /c`, and cmd's argument handling
// has a documented special case for a command line that begins with a quote
// character: unless the ENTIRE line consists of exactly two quote characters
// wrapping a single executable name with nothing special between them (it
// does not here -- there are several separately-quoted arguments), cmd falls
// back to its old behaviour of stripping only the FIRST and LAST quote
// character of the whole line and passing the remainder through unchanged.
// A command shaped like `"C:\...\clang++.exe" -I "..." "..." -o "..."`
// starts and ends with a quote, so that stripping tears the compiler path's
// own closing quote away from its opening one, leaving a dangling quote
// mid-string and breaking the command at the first space inside this
// project's own build environment (`C:\Program Files\LLVM\...`). Verified
// directly against this machine's real compiler path before writing this
// header: the un-wrapped command failed with `'C:/Program' is not
// recognized as an internal or external command`, and wrapping the entire
// command in one more pair of quotes -- so THAT outer pair is what the
// stripping rule consumes, leaving the inner command string untouched --
// fixed it, for both the compile step and the redirected run step below.
inline std::string via_cmd(const std::string& command) { return "\"" + command + "\""; }

// Emits, compiles with the SAME compiler that built this binary, runs, and
// captures stdout. clang++ is already a hard dependency of this project, so
// requiring it here changes nothing; CPython would be a new one and is
// deliberately absent -- see this header's own inclusion by
// codegen_execution_test.cpp for the corpus of expected-output strings, each
// pinned by running real CPython once and checking the answer in, not by
// deriving it here.
inline RunResult compile_and_run(const std::string& python_source) {
    Fixture fixture = build(python_source);
    Emitter emitter(fixture.types, fixture.emit_sink);
    const std::optional<std::string> cpp = emitter.emit_module(*fixture.module);
    EXPECT_TRUE(cpp.has_value()) << "emission must succeed for an end-to-end sample";
    if (!cpp.has_value()) {
        return RunResult{1, ""};
    }

    // Unique per SOURCE TEXT (not merely per test run) so parallel ctest
    // shards racing on the same sample cannot collide on one directory. Not
    // deleted on failure, deliberately: the emitted .cpp is the most useful
    // debugging artifact when one of these tests fails, and a developer
    // rerunning ctest gets a fresh directory anyway since sources differ.
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() /
        ("cythonpp_e2e_" + std::to_string(std::hash<std::string>{}(python_source)));
    std::filesystem::create_directories(directory);
    const std::filesystem::path source = directory / "program.cpp";
    const std::filesystem::path binary = directory / "program.exe";
    { std::ofstream(source) << *cpp; }

    std::ostringstream compile_command;
    compile_command << '"' << CYTHONPP_CXX_COMPILER << "\" -std=c++23 -I \""
                     << CYTHONPP_RUNTIME_DIR << "\" \"" << source.string() << "\" -o \""
                     << binary.string() << '"';
    const int compiled = std::system(via_cmd(compile_command.str()).c_str());
    EXPECT_EQ(compiled, 0) << "emitted C++ must compile:\n" << *cpp;
    if (compiled != 0) {
        return RunResult{1, ""};
    }

    // Redirected to a file rather than captured via a pipe: a pipe needs
    // _popen/read-loop plumbing to also recover the child's exit code, and a
    // plain std::system() + redirect gets both with far less code, at the
    // cost of the intermediate file this function already needs a temp
    // directory for anyway.
    const std::filesystem::path captured = directory / "stdout.txt";
    const std::string run_command = "\"" + binary.string() + "\" > \"" + captured.string() + "\"";
    const int status = std::system(via_cmd(run_command).c_str());

    std::ifstream stream(captured);
    std::ostringstream text;
    text << stream.rdbuf();
    return RunResult{status, text.str()};
}

} // namespace cythonpp::domain::codegen

#endif // CYTHONPP_TESTS_DOMAIN_CODEGEN_COMPILE_AND_RUN_H
