#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "compile_and_run.h"

// The codegen labelled corpus. Every *.py file under CYTHONPP_TEST_FILES_DIR
// /codegen carries a "# stdout:" header naming the exact lines the program
// prints, followed by the real Python source. Task 9's compile_and_run()
// takes that source all the way through the real chain -- Lexer,
// IndentationPass, StatementParser, TypeChecker, Emitter, an external
// clang++ invocation, and finally running the resulting binary -- and this
// harness asserts the captured stdout equals the label.
//
// THE ASYMMETRY, STATED EXPLICITLY (mirroring semantic_corpus_test.cpp's own
// paragraph on this, because the same limit applies here for the same
// reason): `ctest` proves the emitted program matches the RECORDED
// expectation on every run, and it does so hermetically -- nothing in this
// file shells out to Python. It does NOT prove the recorded expectation
// matches CPython. That second claim rests entirely on the developer having
// actually run `python <file>` and pasted the real output into the "#
// stdout:" block by hand; a label that was typed from reasoning about what
// the program "should" print, rather than measured, would sail through this
// harness undetected; a typo in the label would too. The only thing that
// checks a label against real CPython is
// `python scripts/verify_corpus_labels.py --check-codegen`, added in a later
// task, and nothing here invokes it automatically -- every sample's
// trustworthiness rests on that manual discipline, not on this test suite.
// Keep this paragraph accurate if that ever changes (e.g. if CI starts
// running --check-codegen).
namespace cythonpp::domain::codegen {
namespace {

namespace fs = std::filesystem;

std::string strip_eol(std::string line) {
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
        line.pop_back();
    }
    return line;
}

// One sample's parsed header: the exact sequence of lines its program is
// expected to print, each newline-joined by the harness before comparing
// against RunResult::stdout_text.
struct SampleLabel {
    std::vector<std::string> lines;
    // Non-empty when the header itself could not be parsed. A label the
    // harness cannot parse must fail loudly, not be silently skipped or
    // treated as "expect nothing" -- a typo must not make the sample vanish
    // from coverage.
    std::string parse_error;
};

// Parses the leading "# stdout:" block: the FIRST line must read exactly
// "# stdout:", and every following "# "-prefixed (or bare "#", for a blank
// expected output line) line is one more expected output line. The block
// ends at the first line that is neither -- exactly where the real Python
// source begins, matching the semantic corpus's own header convention.
SampleLabel parse_label(const fs::path& path) {
    SampleLabel label;
    std::ifstream file(path);
    if (!file) {
        label.parse_error = "could not open file";
        return label;
    }

    std::string raw_line;
    if (!std::getline(file, raw_line) || strip_eol(raw_line) != "# stdout:") {
        label.parse_error =
            "first line must be exactly '# stdout:', got: " +
            (file.eof() && raw_line.empty() ? std::string("<empty file>") : strip_eol(raw_line));
        return label;
    }

    while (std::getline(file, raw_line)) {
        const std::string line = strip_eol(raw_line);
        if (line == "#") {
            label.lines.emplace_back("");
            continue;
        }
        if (line.rfind("# ", 0) == 0) {
            label.lines.push_back(line.substr(2));
            continue;
        }
        // First non-label line: the header is over, and the rest of the file
        // is the program itself.
        break;
    }
    return label;
}

std::string read_file(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    std::ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

fs::path corpus_dir() {
    return fs::path(CYTHONPP_TEST_FILES_DIR) / "codegen";
}

std::vector<fs::path> corpus_files() {
    std::vector<fs::path> files;
    if (!fs::exists(corpus_dir())) {
        return files;
    }
    for (const fs::directory_entry& entry : fs::directory_iterator(corpus_dir())) {
        if (entry.is_regular_file() && entry.path().extension() == ".py") {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

// Checks ONE sample end-to-end via non-fatal EXPECT_*/ADD_FAILURE, so one bad
// sample never hides a failure elsewhere in the corpus.
void check_sample(const fs::path& path) {
    SCOPED_TRACE("sample: " + path.string());

    const SampleLabel label = parse_label(path);
    if (!label.parse_error.empty()) {
        ADD_FAILURE() << path.string() << ": malformed label header -- " << label.parse_error;
        return;
    }

    std::string expected;
    for (const std::string& line : label.lines) {
        expected += line;
        expected += '\n';
    }

    const std::string source = read_file(path);
    const RunResult result = compile_and_run(source);

    EXPECT_EQ(result.exit_code, 0) << path.string() << ": program must run to completion";
    EXPECT_EQ(result.stdout_text, expected)
        << path.string()
        << ": emitted program's stdout does not match the '# stdout:' label. If this label was "
           "produced by actually running CPython (as it must be -- see this file's own header "
           "comment), then cythonpp itself is wrong here; do not silence this by editing the "
           "label unless you first re-run `python <this file>` and confirm the label was stale.";
}

// A harness that silently finds zero files and passes is the exact failure
// mode this test guards against -- a wrong CYTHONPP_TEST_FILES_DIR, or a
// corpus directory nobody ever populated, produces precisely that.
TEST(CodegenCorpus, CorpusDirectoryIsNotEmpty) {
    const std::vector<fs::path> files = corpus_files();
    ASSERT_FALSE(files.empty())
        << "no *.py files found under " << corpus_dir().string()
        << " -- either the corpus is genuinely empty (add samples under "
           "test_files/codegen/) or CYTHONPP_TEST_FILES_DIR is misconfigured in CMakeLists.txt.";
}

TEST(CodegenCorpus, EverySampleMatchesItsLabel) {
    for (const fs::path& path : corpus_files()) {
        check_sample(path);
    }
}

} // namespace
} // namespace cythonpp::domain::codegen
