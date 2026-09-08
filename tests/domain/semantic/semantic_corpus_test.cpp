#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "domain/ast/module.h"
#include "domain/diagnostics/diagnostic.h"
#include "domain/diagnostics/diagnostic_sink.h"
#include "domain/lexer/indentation_pass.h"
#include "domain/lexer/lexer.h"
#include "domain/parser/statement_parser.h"
#include "domain/semantic/type_checker.h"
#include "domain/semantic/type_map.h"

// The labelled-corpus harness. Every *.py file under CYTHONPP_TEST_FILES_DIR
// /semantic carries a header (see parse_labels below) stating what mypy
// --strict thinks of it and what cythonpp is expected to report for it. This
// is the ONLY mechanism in this project that makes the compliance claim --
// "cythonpp accepts everything mypy --strict accepts" -- falsifiable, rather
// than a claim resting on however many hand-picked unit tests happen to
// exist.
//
// ctest stays hermetic: nothing here shells out to mypy or python. The
// developer-run counterpart is scripts/verify_corpus_labels.py --check-corpus,
// which actually invokes mypy to confirm each "# mypy:" header is honest.
//
// COVERAGE IS ASYMMETRIC, BY CONSTRUCTION, AND THAT IS A REAL LIMIT, NOT AN
// OVERSIGHT. The hard invariant this harness enforces automatically is
// one-directional: "# mypy: clean" + a TypeError/NameError from cythonpp is
// checked right here, on every `ctest` run, with no Python involved. There is
// no equivalent automatic check for the OTHER direction -- a sample labelled
// "# mypy: error ..." that mypy --strict actually accepts. Nothing here can
// tell a correctly-labelled "error" sample apart from one where the label was
// simply typed wrong: a wrong "error" label just silently disables this
// guard for that sample, permanently, and `ctest` will happily pass forever.
// The ONLY thing that catches a wrong "error" label is a human (or CI, which
// this repo does not have) actually running
// `python scripts/verify_corpus_labels.py --check-corpus`, which shells out
// to real mypy. That script is never invoked automatically -- nothing
// enforces that it gets run. Every "error"-labelled sample's trustworthiness
// therefore rests on manual discipline, not on this test suite. Keep this
// paragraph accurate if that ever changes (e.g. if CI starts running
// --check-corpus).
namespace cythonpp::domain::semantic {
namespace {

namespace fs = std::filesystem;

// One parsed "# cythonpp: CODE:LINE:COL message" line.
struct ExpectedDiagnostic {
    std::string code;
    int line = 0;
    int column = 0;
    std::string message;
};

// A sample's parsed header. `expected` is empty for a sample with no
// "# cythonpp:" line at all, meaning cythonpp must report ZERO diagnostics.
struct SampleLabels {
    bool mypy_clean = false;
    std::vector<ExpectedDiagnostic> expected;
    // Non-empty when the header itself could not be parsed. Reported as a
    // hard failure rather than silently treated as "expect nothing": a typo
    // in a label must not make the sample vanish from coverage.
    std::string parse_error;
};

std::string strip_eol(std::string line) {
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
        line.pop_back();
    }
    return line;
}

// Parses one line already known to start with "# cythonpp:". Returns false
// (with `error` explaining why) for anything that does not match
// "CODE:LINE:COL message" -- a label the harness cannot parse must fail
// loudly, not be skipped.
bool parse_cythonpp_line(const std::string& line, ExpectedDiagnostic& out, std::string& error) {
    static const std::string kPrefix = "# cythonpp:";
    std::string rest = line.substr(kPrefix.size());

    const std::size_t first_non_space = rest.find_first_not_of(' ');
    if (first_non_space == std::string::npos) {
        error = "'# cythonpp:' line has nothing after the prefix: " + line;
        return false;
    }
    rest = rest.substr(first_non_space);

    const std::size_t colon1 = rest.find(':');
    const std::size_t colon2 = colon1 == std::string::npos ? std::string::npos
                                                            : rest.find(':', colon1 + 1);
    const std::size_t space = colon2 == std::string::npos ? std::string::npos
                                                           : rest.find(' ', colon2 + 1);
    if (colon1 == std::string::npos || colon2 == std::string::npos ||
        space == std::string::npos) {
        error = "expected 'CODE:LINE:COL message', got: " + line;
        return false;
    }

    out.code = rest.substr(0, colon1);
    const std::string line_str = rest.substr(colon1 + 1, colon2 - colon1 - 1);
    const std::string col_str = rest.substr(colon2 + 1, space - colon2 - 1);
    try {
        std::size_t consumed = 0;
        out.line = std::stoi(line_str, &consumed);
        if (consumed != line_str.size()) throw std::invalid_argument(line_str);
        out.column = std::stoi(col_str, &consumed);
        if (consumed != col_str.size()) throw std::invalid_argument(col_str);
    } catch (const std::exception&) {
        error = "line/column are not plain integers: " + line;
        return false;
    }
    out.message = rest.substr(space + 1);
    return true;
}

// True for exactly "# mypy: error" (no trailing detail) or "# mypy: error "
// followed by freeform detail (informational only -- the harness only ever
// branches on clean-vs-not). A plain prefix match (`rfind(..., 0) == 0`
// without the trailing-space requirement) would also accept a typo like
// "# mypy: errorX" as a valid "error" label -- and "error" is the label that
// DISABLES the invariant guard below, so the lenient match would sit on
// exactly the wrong side: a malformed header would silently turn OFF the one
// check this harness runs on every `ctest` invocation instead of failing
// loudly. Requiring either exact equality or a space after "error" closes
// that hole while still allowing free text after it.
bool is_mypy_error_line(const std::string& line) {
    static const std::string kExact = "# mypy: error";
    static const std::string kPrefix = "# mypy: error ";
    return line == kExact || line.rfind(kPrefix, 0) == 0;
}

// Reads and parses `path`'s header: the FIRST line must be "# mypy: clean" or
// "# mypy: error ..." (freeform after "error", informational only -- the
// harness only ever branches on clean-vs-not), followed by zero or more
// "# cythonpp: ..." lines. The header ends at the first line that is neither,
// exactly where the real source begins -- which is why every label's line
// number counts the header lines too: StatementParser sees the WHOLE file,
// header included, since "#" comments are invisible to it either way.
//
// A "# cythonpp:" line is only ever recognised while still inside that
// leading block. Once the header has ended (the first ordinary source line
// is seen), the REST of the file is still scanned -- not ignored -- purely
// to catch a "# cythonpp:" line placed below the header by mistake. Silently
// dropping such a line would shrink `expected` to fewer entries than the
// file actually documents, which is exactly the "a label the harness cannot
// parse must fail loudly, not be skipped" doctrine this harness claims to
// follow, so a detached label is a parse error, not a no-op.
SampleLabels parse_labels(const fs::path& path) {
    SampleLabels labels;
    std::ifstream file(path);
    if (!file) {
        labels.parse_error = "could not open file";
        return labels;
    }

    std::string raw_line;
    bool seen_mypy_line = false;
    bool in_header = true;
    while (std::getline(file, raw_line)) {
        const std::string line = strip_eol(raw_line);
        if (!seen_mypy_line) {
            if (line == "# mypy: clean") {
                labels.mypy_clean = true;
            } else if (is_mypy_error_line(line)) {
                labels.mypy_clean = false;
            } else {
                labels.parse_error =
                    "first line must be '# mypy: clean' or '# mypy: error ...', got: " + line +
                    " (if this looks identical to a valid header, check for trailing "
                    "whitespace -- an editor auto-save is a common cause, and '# mypy: clean' "
                    "must match exactly)";
                return labels;
            }
            seen_mypy_line = true;
            continue;
        }
        if (in_header && line.rfind("# cythonpp:", 0) == 0) {
            ExpectedDiagnostic diagnostic;
            std::string error;
            if (!parse_cythonpp_line(line, diagnostic, error)) {
                labels.parse_error = error;
                return labels;
            }
            labels.expected.push_back(std::move(diagnostic));
            continue;
        }
        if (in_header) {
            in_header = false; // First non-header line: the header is over.
        }
        // Past the header now (possibly as of this very line). A
        // "# cythonpp:" line here is detached from the header block and
        // would otherwise vanish from `expected` without a trace.
        if (line.rfind("# cythonpp:", 0) == 0) {
            labels.parse_error =
                "'# cythonpp:' line found below the header, detached from the leading "
                "'# mypy:'/'# cythonpp:' block -- move it up next to the other label lines: " +
                line;
            return labels;
        }
    }
    if (!seen_mypy_line) {
        labels.parse_error = "file is empty or has no '# mypy:' header line";
    }
    return labels;
}

std::string read_file(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    std::ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

// The real chain -- source -> Lexer -> IndentationPass -> StatementParser ->
// TypeChecker, ONE sink for both parse and type diagnostics -- exactly what a
// real invocation of cythonpp does. Mirrors type_checker_test.cpp's
// check_module for the same reason that test gives: hand-building a Module
// would let the harness agree with a wrong belief about what the parser
// actually produces.
std::vector<diagnostics::Diagnostic> run_checker(const std::string& source) {
    lexer::Lexer lexer(source);
    const lexer::TokenStream lexed(lexer.tokenize());
    diagnostics::DiagnosticSink sink;
    lexer::TokenStream tokens = lexer::IndentationPass().run(lexed, sink);
    const std::unique_ptr<ast::Module> module =
        parser::StatementParser(tokens, sink).parse_module();
    if (!sink.has_errors()) {
        TypeChecker(sink).check(*module);
    }
    return sink.diagnostics();
}

std::string describe(const std::string& code, int line, int column, const std::string& message) {
    std::ostringstream out;
    out << code << ':' << line << ':' << column << ' ' << message;
    return out.str();
}

std::string describe(const ExpectedDiagnostic& diagnostic) {
    return describe(diagnostic.code, diagnostic.line, diagnostic.column, diagnostic.message);
}

std::string describe(const diagnostics::Diagnostic& diagnostic) {
    return describe(diagnostic.code, diagnostic.line, diagnostic.column, diagnostic.message);
}

fs::path corpus_dir() {
    return fs::path(CYTHONPP_TEST_FILES_DIR) / "semantic";
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

    const SampleLabels labels = parse_labels(path);
    if (!labels.parse_error.empty()) {
        ADD_FAILURE() << path.string() << ": malformed label header -- " << labels.parse_error;
        return;
    }

    // THE RULE THAT EARNS THE WHOLE DESIGN, checked BEFORE anything about
    // what the checker actually produces. The project's hard invariant is
    // "if mypy --strict reports nothing, cythonpp reports no TypeError or
    // NameError". A sample labelled "# mypy: clean" that ALSO expects a
    // TypeError or NameError from cythonpp is a direct contradiction of that
    // invariant on the labels alone -- it must fail by construction, not by
    // whatever the checker happens to produce, so the failure message names
    // the invariant instead of showing a string diff.
    if (labels.mypy_clean) {
        for (const ExpectedDiagnostic& diagnostic : labels.expected) {
            if (diagnostic.code == "TypeError" || diagnostic.code == "NameError") {
                ADD_FAILURE()
                    << path.string() << ": INVARIANT VIOLATED -- labelled '# mypy: clean' "
                    << "but also expects a '" << diagnostic.code << "' from cythonpp ("
                    << describe(diagnostic) << "). The hard invariant this project claims is "
                    << "\"if mypy --strict reports nothing, cythonpp reports no TypeError or "
                    << "NameError\". Either the '# mypy: clean' label is wrong (re-verify with "
                    << "`python scripts/verify_corpus_labels.py --check-corpus`), or cythonpp "
                    << "reporting a " << diagnostic.code << " here is a genuine bug -- fix "
                    << "whichever one is false, do not just edit this label.";
                return;
            }
        }
    }

    const std::string source = read_file(path);
    const std::vector<diagnostics::Diagnostic> actual = run_checker(source);

    bool mismatch = actual.size() != labels.expected.size();
    if (!mismatch) {
        for (std::size_t i = 0; i < actual.size(); ++i) {
            const diagnostics::Diagnostic& got = actual[i];
            const ExpectedDiagnostic& want = labels.expected[i];
            // `got.severity` is deliberately never compared here. The
            // "# cythonpp: CODE:LINE:COL message" label format has no
            // severity field at all -- there is nothing on the `want` side
            // to compare it against. That is a limit of the label format,
            // not an oversight in this condition: an Error silently
            // downgraded to a Warning (same code/line/column/message) would
            // still match.
            if (got.code != want.code || got.line != want.line || got.column != want.column ||
                got.message != want.message) {
                mismatch = true;
                break;
            }
        }
    }

    if (mismatch) {
        std::ostringstream out;
        out << path.string() << ": actual diagnostics do not match the sample's header.\n";
        const std::size_t rows = std::max(actual.size(), labels.expected.size());
        std::vector<std::string> wants(rows);
        std::vector<std::string> gots(rows);
        static const std::string kExpectedHeader = "EXPECTED (from header)";
        std::size_t want_column_width = kExpectedHeader.size();
        for (std::size_t i = 0; i < rows; ++i) {
            wants[i] =
                i < labels.expected.size() ? describe(labels.expected[i]) : std::string("<none>");
            gots[i] = i < actual.size() ? describe(actual[i]) : std::string("<none>");
            want_column_width = std::max(want_column_width, wants[i].size());
        }
        // Pad is computed from the longest row actually present in THIS
        // mismatch, not a fixed constant -- a fixed pad (52 columns, in an
        // earlier version) degrades to a single separating space once an
        // expected message (e.g. NotImplementedError's ~95-char one) runs
        // past it, making exactly the hardest row to read the least
        // readable.
        const std::size_t column_gap = 2;
        out << "  #  " << kExpectedHeader
            << std::string(want_column_width - kExpectedHeader.size() + column_gap, ' ')
            << "ACTUAL (from TypeChecker)\n";
        for (std::size_t i = 0; i < rows; ++i) {
            out << "  " << i << "  " << wants[i]
                << std::string(want_column_width - wants[i].size() + column_gap, ' ') << gots[i]
                << '\n';
        }
        ADD_FAILURE() << out.str();
    }
}

// A harness that silently finds zero files and passes is the exact failure
// mode this test guards against -- a wrong CYTHONPP_TEST_FILES_DIR produces
// precisely that.
TEST(SemanticCorpus, CorpusDirectoryIsNotEmpty) {
    const std::vector<fs::path> files = corpus_files();
    ASSERT_FALSE(files.empty())
        << "no *.py files found under " << corpus_dir().string()
        << " -- either the corpus is genuinely empty (add samples under "
           "test_files/semantic/) or CYTHONPP_TEST_FILES_DIR is misconfigured "
           "in CMakeLists.txt.";
}

TEST(SemanticCorpus, EverySampleMatchesItsLabels) {
    for (const fs::path& path : corpus_files()) {
        check_sample(path);
    }
}

} // namespace
} // namespace cythonpp::domain::semantic
