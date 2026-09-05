# IndentationPass Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Recover Python block structure from the lexer's per-character `SPACE`/`TAB` tokens, emitting `INDENT`/`DEDENT` and reporting `TabError`/`IndentationError`, so a parser becomes possible.

**Architecture:** A new `IndentationPass` in `domain/lexer/` maps `TokenStream → TokenStream`, deleting leading whitespace runs and inserting `INDENT`/`DEDENT` before the first significant token of each logical line. It is total — never throws — and always returns a stream where `#INDENT == #DEDENT`. Errors go to a new `domain::diagnostics::DiagnosticSink`, which `CompilePipeline` drains into `ports::DiagnosticsReporter` (wired for the first time).

**Tech Stack:** C++17, clang++, CMake + Ninja, GoogleTest (fetched via `FetchContent`).

**Spec:** `docs/superpowers/specs/2026-08-08-indentation-pass-design.md`

## Global Constraints

- C++17. No new third-party dependencies.
- `CMakeLists.txt` uses **explicit source lists, not globs**. Every new `.cpp` must be added to `CYTHONPP_LIB_SOURCES` or the `cythonpp_tests` source list by hand, or it silently will not build.
- One class or enum per file. Namespaces mirror the folder structure under `src/`.
- Cross-module includes are rooted at `src/` (`#include "domain/lexer/token.h"`). Same-directory includes use a plain relative quote-include.
- Header guards follow `CYTHONPP_<PATH>_<FILE>_H`.
- `Lexer` and `lexer.cpp` are **not modified by this plan**. Every existing lexer test must still pass unchanged.
- Comments explain *why*, not *what* — match the density and tone of `token_type.h` and `scan_context.h`.
- The balance invariant `#INDENT == #DEDENT` holds for every input, including malformed input. The parser will depend on it.

---

### Task 1: Diagnostic and DiagnosticSink

**Files:**
- Create: `src/domain/diagnostics/diagnostic.h`
- Create: `src/domain/diagnostics/diagnostic_sink.h`
- Create: `src/domain/diagnostics/diagnostic_sink.cpp`
- Test: `tests/domain/diagnostics/diagnostic_sink_test.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: nothing.
- Produces: `cythonpp::domain::diagnostics::Severity` (`Warning`, `Error`); `struct Diagnostic { Severity severity; std::string code; std::string message; int line; int column; }`; `class DiagnosticSink` with `void report(Diagnostic)`, `void report_error(std::string code, std::string message, int line, int column)`, `const std::vector<Diagnostic>& diagnostics() const`, `bool has_errors() const`, `bool empty() const`.

- [ ] **Step 1: Write the failing test**

Create `tests/domain/diagnostics/diagnostic_sink_test.cpp`:

```cpp
#include <gtest/gtest.h>

#include <string>

#include "domain/diagnostics/diagnostic_sink.h"

namespace cythonpp::domain::diagnostics {
namespace {

TEST(DiagnosticSink, StartsEmpty) {
    DiagnosticSink sink;
    EXPECT_TRUE(sink.empty());
    EXPECT_FALSE(sink.has_errors());
    EXPECT_TRUE(sink.diagnostics().empty());
}

TEST(DiagnosticSink, RecordsSeverityCodeMessageAndPosition) {
    DiagnosticSink sink;
    sink.report_error("TabError", "inconsistent use of tabs and spaces in indentation", 7, 3);

    ASSERT_EQ(sink.diagnostics().size(), 1u);
    const Diagnostic& diagnostic = sink.diagnostics().front();
    EXPECT_EQ(diagnostic.severity, Severity::Error);
    EXPECT_EQ(diagnostic.code, "TabError");
    EXPECT_EQ(diagnostic.message, "inconsistent use of tabs and spaces in indentation");
    EXPECT_EQ(diagnostic.line, 7);
    EXPECT_EQ(diagnostic.column, 3);
}

TEST(DiagnosticSink, PreservesReportOrder) {
    DiagnosticSink sink;
    sink.report_error("First", "first", 1, 1);
    sink.report_error("Second", "second", 2, 1);
    sink.report_error("Third", "third", 3, 1);

    ASSERT_EQ(sink.diagnostics().size(), 3u);
    EXPECT_EQ(sink.diagnostics()[0].code, "First");
    EXPECT_EQ(sink.diagnostics()[1].code, "Second");
    EXPECT_EQ(sink.diagnostics()[2].code, "Third");
}

TEST(DiagnosticSink, WarningsDoNotCountAsErrors) {
    DiagnosticSink sink;
    sink.report(Diagnostic{Severity::Warning, "Advice", "consider not doing that", 1, 1});

    EXPECT_FALSE(sink.empty());
    EXPECT_FALSE(sink.has_errors());
}

} // namespace
} // namespace cythonpp::domain::diagnostics
```

- [ ] **Step 2: Add both new files to the build**

In `CMakeLists.txt`, add to `CYTHONPP_LIB_SOURCES` (after the `src/domain/lexer/lexer.cpp` line):

```cmake
    src/domain/diagnostics/diagnostic_sink.cpp
```

And to the `cythonpp_tests` source list:

```cmake
        tests/domain/diagnostics/diagnostic_sink_test.cpp
```

No `target_include_directories` change is needed — `src` is already `PUBLIC` on the library and `PRIVATE` on the tests. No ctest registration is needed — `gtest_discover_tests` enumerates at build time.

- [ ] **Step 3: Run the test to verify it fails**

Run: `cmake -S . -B build -G Ninja && cmake --build build`
Expected: FAIL to compile — `domain/diagnostics/diagnostic_sink.h` does not exist.

- [ ] **Step 4: Write `src/domain/diagnostics/diagnostic.h`**

```cpp
#ifndef CYTHONPP_DOMAIN_DIAGNOSTICS_DIAGNOSTIC_H
#define CYTHONPP_DOMAIN_DIAGNOSTICS_DIAGNOSTIC_H

#include <string>

namespace cythonpp::domain::diagnostics {

// Warning/Error rather than the SCREAMING_CASE used by token_category and
// token_type, deliberately: ERROR is an object-like macro in <wingdi.h>, and
// macro substitution happens before scoping, so Severity::ERROR would fail to
// compile in any translation unit that ever pulls in <windows.h>. The
// SCREAMING enums elsewhere are bit-flag and packed-value constants where the
// C-style spelling carries meaning; this is an ordinary scoped enum.
enum class Severity { Warning, Error };

// One problem found in one source file.
//
// `code` holds the Python exception name -- "TabError", "IndentationError" --
// so a later front end can match CPython's wording, and so a caller can branch
// on the kind of problem without parsing `message`.
//
// The source path is deliberately absent. A domain pass only ever sees one
// file's tokens and has no way to know its path; the caller that does knows
// attaches it on the way out, at ports::DiagnosticsReporter.
struct Diagnostic {
    Severity severity;
    std::string code;
    std::string message;
    int line;
    int column;
};

} // namespace cythonpp::domain::diagnostics

#endif // CYTHONPP_DOMAIN_DIAGNOSTICS_DIAGNOSTIC_H
```

- [ ] **Step 5: Write `src/domain/diagnostics/diagnostic_sink.h`**

```cpp
#ifndef CYTHONPP_DOMAIN_DIAGNOSTICS_DIAGNOSTIC_SINK_H
#define CYTHONPP_DOMAIN_DIAGNOSTICS_DIAGNOSTIC_SINK_H

#include <string>
#include <vector>

#include "diagnostic.h"

namespace cythonpp::domain::diagnostics {

// Collects the diagnostics a pass produces, in report order.
//
// A concrete class rather than an interface: domain code must not depend on
// ports/, and a pass has no business knowing whether its output is printed,
// buffered, or discarded. The application layer drains a sink into
// ports::DiagnosticsReporter, which is where the choice belongs.
class DiagnosticSink {
public:
    void report(Diagnostic diagnostic);

    // The common case, spelled so call sites stay off aggregate initialization
    // -- adding a field to Diagnostic should not break every reporting pass.
    void report_error(std::string code, std::string message, int line, int column);

    const std::vector<Diagnostic>& diagnostics() const;

    // True if any diagnostic has Severity::Error. Distinct from !empty(),
    // which a warning also satisfies.
    bool has_errors() const;
    bool empty() const;

private:
    std::vector<Diagnostic> diagnostics_;
};

} // namespace cythonpp::domain::diagnostics

#endif // CYTHONPP_DOMAIN_DIAGNOSTICS_DIAGNOSTIC_SINK_H
```

- [ ] **Step 6: Write `src/domain/diagnostics/diagnostic_sink.cpp`**

```cpp
#include "domain/diagnostics/diagnostic_sink.h"

#include <algorithm>
#include <utility>

namespace cythonpp::domain::diagnostics {

void DiagnosticSink::report(Diagnostic diagnostic) {
    diagnostics_.push_back(std::move(diagnostic));
}

void DiagnosticSink::report_error(std::string code, std::string message, int line, int column) {
    report(Diagnostic{Severity::Error, std::move(code), std::move(message), line, column});
}

const std::vector<Diagnostic>& DiagnosticSink::diagnostics() const { return diagnostics_; }

bool DiagnosticSink::has_errors() const {
    return std::any_of(diagnostics_.begin(), diagnostics_.end(),
                       [](const Diagnostic& diagnostic) { return diagnostic.severity == Severity::Error; });
}

bool DiagnosticSink::empty() const { return diagnostics_.empty(); }

} // namespace cythonpp::domain::diagnostics
```

- [ ] **Step 7: Run the tests to verify they pass**

Run: `cmake --build build && ctest --test-dir build -R "DiagnosticSink" --output-on-failure`
Expected: 4 tests, all PASS.

- [ ] **Step 8: Commit**

```bash
git add CMakeLists.txt src/domain/diagnostics tests/domain/diagnostics
git commit -m "Add Diagnostic and DiagnosticSink domain types"
```

---

### Task 2: INDENT and DEDENT token types

**Files:**
- Modify: `src/domain/lexer/token_type.h` (the `SPECIAL_FLAGS` block and its preceding comment)
- Modify: `src/domain/lexer/token_type_name.cpp` (after `case token_type::TAB:`)
- Test: `tests/domain/lexer/token_test.cpp` (the `ALL_TOKEN_TYPES` array and the count assertion)

**Interfaces:**
- Consumes: nothing.
- Produces: `token_type::INDENT` and `token_type::DEDENT`, both carrying `token_category::SPECIAL` only.

This task is the enumerator-addition ritual from `CLAUDE.md`. Its three edits must land together — editing the array without the count assertion leaves the build red, and editing the enum without `token_type_name` leaves `TokenType.EveryEnumeratorHasAName` red.

- [ ] **Step 1: Write the failing test**

In `tests/domain/lexer/token_test.cpp`, add to `ALL_TOKEN_TYPES` immediately after `token_type::TAB,`:

```cpp
    token_type::INDENT,
    token_type::DEDENT,
```

Change the count assertion in `TEST(TokenType, EveryEnumeratorHasAUniqueValue)` from `EXPECT_EQ(count, 115u);` to:

```cpp
    EXPECT_EQ(count, 117u);
```

Then add a new test at the end of the file, before the closing `} // namespace`:

```cpp
TEST(TokenCategory, IndentAndDedentAreSpecialOnly) {
    // Structure markers, not values: nothing in the stream binds to them, so
    // an "is this an object?" check must not pick them up.
    EXPECT_TRUE(has_category(token_type::INDENT, token_category::SPECIAL));
    EXPECT_TRUE(has_category(token_type::DEDENT, token_category::SPECIAL));
    EXPECT_FALSE(has_category(token_type::INDENT, token_category::OBJECT));
    EXPECT_FALSE(has_category(token_type::DEDENT, token_category::OBJECT));
    EXPECT_NE(token_type::INDENT, token_type::DEDENT);
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build`
Expected: FAIL to compile — `'INDENT' is not a member of 'cythonpp::domain::lexer::token_type'`.

- [ ] **Step 3: Add the enumerators**

In `src/domain/lexer/token_type.h`, replace the comment and block that currently reads:

```cpp
    // Special: stream markers, never a Python value. INDENT/DEDENT are
    // deliberately absent -- indentation is not modelled yet, and the
    // scanner emits SPACE/TAB per leading character instead.
    TOKEN_EOF   = detail::make_token(detail::SPECIAL_FLAGS, 0),
    TOKEN_ERROR = detail::make_token(detail::SPECIAL_FLAGS, 1),
    NEWLINE     = detail::make_token(detail::SPECIAL_FLAGS, 2),
    SPACE       = detail::make_token(detail::SPECIAL_FLAGS, 3),
    TAB         = detail::make_token(detail::SPECIAL_FLAGS, 4),
```

with:

```cpp
    // Special: stream markers, never a Python value.
    //
    // SPACE/TAB are what the scanner emits, one per leading character.
    // INDENT/DEDENT are what IndentationPass replaces them with, so the two
    // pairs never coexist in the same stream: a stream that has been through
    // the pass has no SPACE or TAB in it at all.
    TOKEN_EOF   = detail::make_token(detail::SPECIAL_FLAGS, 0),
    TOKEN_ERROR = detail::make_token(detail::SPECIAL_FLAGS, 1),
    NEWLINE     = detail::make_token(detail::SPECIAL_FLAGS, 2),
    SPACE       = detail::make_token(detail::SPECIAL_FLAGS, 3),
    TAB         = detail::make_token(detail::SPECIAL_FLAGS, 4),
    INDENT      = detail::make_token(detail::SPECIAL_FLAGS, 5),
    DEDENT      = detail::make_token(detail::SPECIAL_FLAGS, 6),
```

- [ ] **Step 4: Add the names**

In `src/domain/lexer/token_type_name.cpp`, after `case token_type::TAB: return "TAB";`:

```cpp
        case token_type::INDENT: return "INDENT";
        case token_type::DEDENT: return "DEDENT";
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cmake --build build && ctest --test-dir build -R "TokenType|TokenCategory" --output-on-failure`
Expected: all PASS, including `TokenType.EveryEnumeratorHasAUniqueValue` and `TokenType.EveryEnumeratorHasAName`.

- [ ] **Step 6: Commit**

```bash
git add src/domain/lexer/token_type.h src/domain/lexer/token_type_name.cpp tests/domain/lexer/token_test.cpp
git commit -m "Add INDENT and DEDENT token types"
```

---

### Task 3: IndentationPass core — whitespace removal, levels, EOF flush

**Files:**
- Create: `src/domain/lexer/indentation_pass.h`
- Create: `src/domain/lexer/indentation_pass.cpp`
- Test: `tests/domain/lexer/indentation_pass_test.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `token_type::INDENT` / `DEDENT` (Task 2); `diagnostics::DiagnosticSink` (Task 1).
- Produces: `class IndentationPass` with `TokenStream run(const TokenStream& tokens, diagnostics::DiagnosticSink& sink) const`.

This task implements the whole algorithm including the error paths, because the error paths are structurally inseparable from the comparison — but only asserts the non-error behaviour. Tasks 4 and 5 add the tests that pin the error paths.

- [ ] **Step 1: Write the failing test**

Create `tests/domain/lexer/indentation_pass_test.cpp`:

```cpp
#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "domain/diagnostics/diagnostic_sink.h"
#include "domain/lexer/indentation_pass.h"
#include "domain/lexer/lexer.h"
#include "domain/lexer/token_type_name.h"

namespace cythonpp::domain::lexer {
namespace {

using TypeList = std::vector<token_type>;

int count_of(const TypeList& types, token_type wanted) {
    return static_cast<int>(std::count(types.begin(), types.end(), wanted));
}

// The invariant the parser will depend on. Checked on every fixture in this
// file, including the malformed ones, because the recovery paths are exactly
// where a regression would hide and no individual expectation would catch it.
void expect_balanced(const TypeList& types) {
    EXPECT_EQ(count_of(types, token_type::INDENT), count_of(types, token_type::DEDENT))
        << "INDENT/DEDENT counts differ";
    EXPECT_EQ(count_of(types, token_type::SPACE), 0) << "SPACE survived the pass";
    EXPECT_EQ(count_of(types, token_type::TAB), 0) << "TAB survived the pass";
}

TokenStream pass(const std::string& source, diagnostics::DiagnosticSink& sink) {
    return IndentationPass().run(TokenStream(Lexer(source).tokenize()), sink);
}

// Runs the pass and asserts the source produced no diagnostics. Most fixtures
// are well-formed, so this keeps them to one line.
TypeList types_of(const std::string& source) {
    diagnostics::DiagnosticSink sink;
    TypeList types;
    for (const Token& token : pass(source, sink)) {
        types.push_back(token.type());
    }
    EXPECT_TRUE(sink.empty()) << "unexpected diagnostic count: " << sink.diagnostics().size();
    expect_balanced(types);
    return types;
}

// For fixtures that are expected to produce diagnostics.
TypeList types_of(const std::string& source, diagnostics::DiagnosticSink& sink) {
    TypeList types;
    for (const Token& token : pass(source, sink)) {
        types.push_back(token.type());
    }
    expect_balanced(types);
    return types;
}

TEST(IndentationPass, EmptyStreamPassesThroughUnchanged) {
    diagnostics::DiagnosticSink sink;
    const TokenStream result = IndentationPass().run(TokenStream(), sink);

    EXPECT_TRUE(result.empty());
    EXPECT_TRUE(sink.empty());
}

TEST(IndentationPass, UnindentedFileGainsNoIndentOrDedent) {
    EXPECT_EQ(types_of("x = 1\n"),
              (TypeList{token_type::IDENTIFIER, token_type::OP_ASSIGN, token_type::LITERAL_INT,
                        token_type::NEWLINE, token_type::TOKEN_EOF}));
}

TEST(IndentationPass, WhitespaceOnlyFileGainsNoIndentOrDedent) {
    EXPECT_EQ(types_of("   \n\t\n  "), (TypeList{token_type::TOKEN_EOF}));
}

TEST(IndentationPass, CommentOnlyFileKeepsTheCommentAndGainsNothing) {
    EXPECT_EQ(types_of("# note\n"), (TypeList{token_type::COMMENT_SINGLE, token_type::TOKEN_EOF}));
}

TEST(IndentationPass, LeadingWhitespaceIsReplacedByIndentBeforeTheFirstSignificantToken) {
    // The INDENT sits after the NEWLINE that opened the line, not before it,
    // so a parser reading "NEWLINE then INDENT" sees a block start.
    EXPECT_EQ(types_of("if x:\n    y\n"),
              (TypeList{token_type::KEYWORD_IF, token_type::IDENTIFIER, token_type::COLON,
                        token_type::NEWLINE, token_type::INDENT, token_type::IDENTIFIER,
                        token_type::NEWLINE, token_type::DEDENT, token_type::TOKEN_EOF}));
}

TEST(IndentationPass, OneIndentIsEmittedPerLevelRegardlessOfWidth) {
    // A jump of eight columns is one level, not eight.
    EXPECT_EQ(count_of(types_of("if x:\n        y\n"), token_type::INDENT), 1);
}

TEST(IndentationPass, DedentToColumnZeroIsDetectedDespiteNoWhitespaceTokensExisting) {
    // The dedenting line has no SPACE tokens at all, so the comparison has to
    // run once per logical line rather than only when a run was observed.
    EXPECT_EQ(types_of("if x:\n    y\nz\n"),
              (TypeList{token_type::KEYWORD_IF, token_type::IDENTIFIER, token_type::COLON,
                        token_type::NEWLINE, token_type::INDENT, token_type::IDENTIFIER,
                        token_type::NEWLINE, token_type::DEDENT, token_type::IDENTIFIER,
                        token_type::NEWLINE, token_type::TOKEN_EOF}));
}

TEST(IndentationPass, DedentingSeveralLevelsAtOnceEmitsOneDedentPerLevel) {
    const TypeList types = types_of("if a:\n    if b:\n        c\nd\n");
    EXPECT_EQ(count_of(types, token_type::INDENT), 2);
    EXPECT_EQ(count_of(types, token_type::DEDENT), 2);
}

TEST(IndentationPass, EqualIndentationEmitsNeitherIndentNorDedent) {
    const TypeList types = types_of("if a:\n    x\n    y\n    z\n");
    EXPECT_EQ(count_of(types, token_type::INDENT), 1);
    EXPECT_EQ(count_of(types, token_type::DEDENT), 1);
}

TEST(IndentationPass, AllOpenLevelsAreFlushedAsDedentsBeforeEndOfFile) {
    const TypeList types = types_of("if a:\n    if b:\n        c\n");
    EXPECT_EQ(count_of(types, token_type::DEDENT), 2);
    ASSERT_GE(types.size(), 3u);
    EXPECT_EQ(types[types.size() - 1], token_type::TOKEN_EOF);
    EXPECT_EQ(types[types.size() - 2], token_type::DEDENT);
    EXPECT_EQ(types[types.size() - 3], token_type::DEDENT);
}

TEST(IndentationPass, IndentedFirstLineOfTheFileIsMeasured) {
    // There is no preceding NEWLINE, so a loop keyed on "just saw a NEWLINE"
    // would miss this line's indentation entirely.
    const TypeList types = types_of("    x = 1\n");
    EXPECT_EQ(count_of(types, token_type::INDENT), 1);
    EXPECT_EQ(types.front(), token_type::INDENT);
}

TEST(IndentationPass, SynthesizedTokensCarryThePositionOfTheTokenTheyPrecede) {
    diagnostics::DiagnosticSink sink;
    const TokenStream result = pass("if x:\n    y\n", sink);

    const Token& indent = result.at(4);
    ASSERT_EQ(indent.type(), token_type::INDENT);
    EXPECT_EQ(indent.line_number(), 2);
    EXPECT_EQ(indent.column_number(), 5);
    // Synthesized, so an empty lexeme -- the same marker finish() uses for
    // the NEWLINE it invents at end of file.
    EXPECT_EQ(indent.lexeme(), "");
}

TEST(IndentationPass, RunningTheSamePassObjectTwiceProducesTheSameResult) {
    const IndentationPass indentation;
    const TokenStream lexed(Lexer("if a:\n    x\n").tokenize());

    diagnostics::DiagnosticSink first_sink;
    diagnostics::DiagnosticSink second_sink;
    const TokenStream first = indentation.run(lexed, first_sink);
    const TokenStream second = indentation.run(lexed, second_sink);

    EXPECT_EQ(first.size(), second.size());
    EXPECT_TRUE(first_sink.empty());
    EXPECT_TRUE(second_sink.empty());
}

} // namespace
} // namespace cythonpp::domain::lexer
```

- [ ] **Step 2: Add the new files to the build**

In `CMakeLists.txt`, add to `CYTHONPP_LIB_SOURCES` after `src/domain/lexer/lexer.cpp`:

```cmake
    src/domain/lexer/indentation_pass.cpp
```

And to the `cythonpp_tests` source list:

```cmake
        tests/domain/lexer/indentation_pass_test.cpp
```

- [ ] **Step 3: Run the test to verify it fails**

Run: `cmake -S . -B build -G Ninja && cmake --build build`
Expected: FAIL to compile — `domain/lexer/indentation_pass.h` does not exist.

- [ ] **Step 4: Write `src/domain/lexer/indentation_pass.h`**

```cpp
#ifndef CYTHONPP_DOMAIN_LEXER_INDENTATION_PASS_H
#define CYTHONPP_DOMAIN_LEXER_INDENTATION_PASS_H

#include "domain/diagnostics/diagnostic_sink.h"
#include "token_stream.h"

namespace cythonpp::domain::lexer {

// Replaces the scanner's per-character SPACE/TAB runs with INDENT/DEDENT
// tokens, recovering the block structure Python's grammar is built on.
//
// A separate pass rather than logic inside Lexer, which is what CPython does:
// the scanner deliberately preserves which whitespace character was used so
// that Python's tab rules can be applied here, and keeping the two apart means
// this can be tested by feeding it a token vector instead of round-tripping
// through source strings.
//
// Guarantees, both of which the parser will lean on:
//
//  - Total. An unbalanced dedent or an ambiguous tab is a diagnostic, never an
//    exception, so a caller needs no try/catch and still gets a usable stream.
//  - Balanced. The output always contains exactly as many DEDENTs as INDENTs,
//    even for input that is malformed in every way at once.
//
// Input is expected to come straight from Lexer::tokenize(). The contract it
// relies on: SPACE/TAB appear only as a contiguous run at the start of a
// logical line that has content, NEWLINE appears only at the end of a logical
// line, and blank and comment-only lines contribute no indentation.
class IndentationPass {
public:
    // Diagnostics are appended to `sink` in source order. The sink is a
    // parameter rather than a member so that running the pass twice cannot
    // accumulate duplicate diagnostics.
    TokenStream run(const TokenStream& tokens, diagnostics::DiagnosticSink& sink) const;
};

} // namespace cythonpp::domain::lexer

#endif // CYTHONPP_DOMAIN_LEXER_INDENTATION_PASS_H
```

- [ ] **Step 5: Write `src/domain/lexer/indentation_pass.cpp`**

```cpp
#include "domain/lexer/indentation_pass.h"

#include <utility>
#include <vector>

#include "token.h"
#include "token_type.h"

namespace cythonpp::domain::lexer {

namespace {

// CPython's col/altcol pair. `col` expands a tab to the next tab stop;
// `alt_col` counts a tab as a single column. Indentation whose ordering
// depends on which of the two you believe is precisely what TabError means,
// which is why both are carried rather than picking one.
struct IndentLevel {
    int col = 0;
    int alt_col = 0;
};

constexpr int TAB_STOP = 8;

class Builder {
public:
    explicit Builder(diagnostics::DiagnosticSink& sink) : sink_(sink) {
        // The sentinel. Never popped and never overwritten: every DEDENT has
        // to answer to an INDENT, and the base level never had one.
        levels_.push_back(IndentLevel{});
    }

    std::vector<Token> run(const std::vector<Token>& tokens);

private:
    void close_line(const Token& next);
    void report_tab_error(const Token& at);
    void emit(token_type type, const Token& at);
    void flush(const Token& at);

    diagnostics::DiagnosticSink& sink_;
    std::vector<IndentLevel> levels_;
    std::vector<Token> out_;
    IndentLevel pending_;
    // True at the start of the file as well as after every NEWLINE: the first
    // logical line has no NEWLINE in front of it but still has indentation.
    bool measuring_ = true;
};

std::vector<Token> Builder::run(const std::vector<Token>& tokens) {
    out_.reserve(tokens.size());

    for (const Token& token : tokens) {
        switch (token.type()) {
            case token_type::SPACE:
                pending_.col += 1;
                pending_.alt_col += 1;
                continue; // whitespace never reaches the output
            case token_type::TAB:
                // Advance to the next tab stop, not add eight: "  \t" is
                // column 8, not 10.
                pending_.col = (pending_.col / TAB_STOP + 1) * TAB_STOP;
                pending_.alt_col += 1;
                continue;
            case token_type::COMMENT_SINGLE:
                // Trivia. A comment-only line emits no NEWLINE and no
                // whitespace, so it is invisible as a line boundary and must
                // neither open nor close one.
                out_.push_back(token);
                continue;
            case token_type::NEWLINE:
                out_.push_back(token);
                pending_ = IndentLevel{};
                measuring_ = true;
                continue;
            case token_type::TOKEN_EOF:
                flush(token);
                out_.push_back(token);
                continue;
            default:
                break;
        }

        // The first significant token of a logical line, and so the point at
        // which the measurement accumulated above gets spent. Note the run and
        // this token can be on different physical lines -- a backslash
        // continuation puts them one apart -- so lines are grouped by token
        // adjacency and never by line_number().
        if (measuring_) {
            close_line(token);
        }
        out_.push_back(token);
    }

    // A well-formed stream ends in TOKEN_EOF and was flushed above. Flush
    // again for one that does not, because the balance guarantee is
    // unconditional. levels_ being deeper than the sentinel implies an INDENT
    // was emitted, which implies out_ is non-empty.
    if (levels_.size() > 1) {
        flush(out_.back());
    }

    return std::move(out_);
}

void Builder::close_line(const Token& next) {
    measuring_ = false;
    const IndentLevel line = pending_;

    if (line.col == levels_.back().col) {
        if (line.alt_col != levels_.back().alt_col) {
            report_tab_error(next);
        }
        return;
    }

    if (line.col > levels_.back().col) {
        if (line.alt_col <= levels_.back().alt_col) {
            report_tab_error(next);
        }
        // Pushed even after a TabError: recovery continues under the `col`
        // interpretation, and col is strictly increasing here, so the stack
        // stays ordered and the stream stays balanced.
        levels_.push_back(line);
        emit(token_type::INDENT, next);
        return;
    }

    // Guarded on size() > 1 so the sentinel is never popped. An unguarded pop
    // here is undefined behaviour, not merely a bad error message.
    while (levels_.size() > 1 && line.col < levels_.back().col) {
        levels_.pop_back();
        emit(token_type::DEDENT, next);
    }

    if (line.col != levels_.back().col) {
        sink_.report_error("IndentationError", "unindent does not match any outer indentation level",
                           next.line_number(), next.column_number());
        // Accept this line as the current level so one bad line does not
        // cascade into every line below it. Only ever above the sentinel:
        // overwriting the base would make it poppable, and the next dedent
        // would then emit a DEDENT with no INDENT to answer to.
        if (levels_.size() > 1) {
            levels_.back() = line;
        }
        return;
    }

    if (line.alt_col != levels_.back().alt_col) {
        report_tab_error(next);
    }
}

void Builder::report_tab_error(const Token& at) {
    sink_.report_error("TabError", "inconsistent use of tabs and spaces in indentation",
                       at.line_number(), at.column_number());
}

void Builder::emit(token_type type, const Token& at) {
    // Empty lexeme marks a synthesized token, the same convention the scanner
    // uses for the NEWLINE it invents at end of file.
    out_.emplace_back(type, "", at.line_number(), at.column_number());
}

void Builder::flush(const Token& at) {
    // size() - 1 dedents, not size(): the sentinel never had a matching INDENT.
    while (levels_.size() > 1) {
        levels_.pop_back();
        emit(token_type::DEDENT, at);
    }
}

} // namespace

TokenStream IndentationPass::run(const TokenStream& tokens, diagnostics::DiagnosticSink& sink) const {
    return TokenStream(Builder(sink).run(tokens.tokens()));
}

} // namespace cythonpp::domain::lexer
```

- [ ] **Step 6: Run the tests to verify they pass**

Run: `cmake --build build && ctest --test-dir build -R "IndentationPass" --output-on-failure`
Expected: 13 tests, all PASS.

- [ ] **Step 7: Run the whole suite to confirm nothing regressed**

Run: `ctest --test-dir build --output-on-failure`
Expected: all PASS. The lexer tests in particular must be untouched — `lexer.cpp` was not modified.

Note there is no `is_significant` helper. The `switch` in `run` names every token type that is *not* the first significant token of a line and `continue`s on each, so the `default:` branch already is the predicate. Adding a named helper alongside it would give the same rule two homes that could drift apart. Note also this is not the scanner's notion of significance (`lexer.cpp:102-103` counts `NEWLINE` as significant, because it is answering "does this line have content?").

- [ ] **Step 8: Commit**

```bash
git add CMakeLists.txt src/domain/lexer/indentation_pass.h src/domain/lexer/indentation_pass.cpp tests/domain/lexer/indentation_pass_test.cpp
git commit -m "Add IndentationPass emitting INDENT and DEDENT"
```

---

### Task 4: Tab arithmetic and TabError

**Files:**
- Test: `tests/domain/lexer/indentation_pass_test.cpp` (append)

**Interfaces:**
- Consumes: `IndentationPass::run` and the `types_of` / `count_of` / `expect_balanced` helpers from Task 3.
- Produces: nothing new. This task pins behaviour Task 3 already implemented.

- [ ] **Step 1: Write the failing tests**

Append to `tests/domain/lexer/indentation_pass_test.cpp`, before the closing `} // namespace`:

```cpp
TEST(IndentationPass, ConsistentTabIndentationProducesNoDiagnostic) {
    const TypeList types = types_of("if a:\n\tx\n\t\ty\n");
    EXPECT_EQ(count_of(types, token_type::INDENT), 2);
    EXPECT_EQ(count_of(types, token_type::DEDENT), 2);
}

TEST(IndentationPass, TabAdvancesToTheNextTabStopRatherThanAddingEight) {
    // "  \t" is column 8. If a tab added eight instead it would be column 10,
    // and the eight-space line below would compare as a dedent rather than as
    // the same column -- so the TabError here is what proves the arithmetic.
    diagnostics::DiagnosticSink sink;
    const TypeList types = types_of("if a:\n  \tx\n        y\n", sink);

    EXPECT_EQ(count_of(types, token_type::INDENT), 1);
    ASSERT_EQ(sink.diagnostics().size(), 1u);
    EXPECT_EQ(sink.diagnostics().front().code, "TabError");
}

TEST(IndentationPass, EqualColumnsWithADifferentTabAndSpaceMixReportTabError) {
    // "\t" and eight spaces are both column 8, but alt column 1 and 8. Which
    // block the second line belongs to depends on the tab width, so Python
    // refuses to guess.
    diagnostics::DiagnosticSink sink;
    types_of("if a:\n\tx\n        y\n", sink);

    ASSERT_EQ(sink.diagnostics().size(), 1u);
    EXPECT_EQ(sink.diagnostics().front().code, "TabError");
    EXPECT_EQ(sink.diagnostics().front().severity, diagnostics::Severity::Error);
    EXPECT_EQ(sink.diagnostics().front().line, 3);
}

TEST(IndentationPass, DeeperColumnWithNonIncreasingAltColumnReportsTabError) {
    // Eight spaces is col 8 / alt 8; two tabs is col 16 / alt 2. Deeper by one
    // measure, shallower by the other.
    diagnostics::DiagnosticSink sink;
    types_of("if a:\n        x\n\t\ty\n", sink);

    ASSERT_GE(sink.diagnostics().size(), 1u);
    EXPECT_EQ(sink.diagnostics().front().code, "TabError");
}

TEST(IndentationPass, TabErrorRecoveryStillEmitsABalancedStream) {
    // expect_balanced runs inside types_of, so this asserts the recovery path
    // does not leak a level. Named explicitly because it is the property, not
    // the token counts, that matters here.
    diagnostics::DiagnosticSink sink;
    const TypeList types = types_of("if a:\n\tx\n        y\n\t\tz\nw\n", sink);
    EXPECT_FALSE(sink.empty());
    EXPECT_EQ(count_of(types, token_type::INDENT), count_of(types, token_type::DEDENT));
}

TEST(IndentationPass, TabErrorDiagnosticsCarryTheOffendingPosition) {
    diagnostics::DiagnosticSink sink;
    types_of("if a:\n\tx\n        y\n", sink);

    ASSERT_EQ(sink.diagnostics().size(), 1u);
    // The position of the token the diagnostic is about, so an editor jumps to
    // the statement rather than to the invisible whitespace before it.
    EXPECT_EQ(sink.diagnostics().front().line, 3);
    EXPECT_EQ(sink.diagnostics().front().column, 9);
}
```

- [ ] **Step 2: Run the tests**

Run: `cmake --build build && ctest --test-dir build -R "IndentationPass" --output-on-failure`
Expected: all PASS — Task 3 already implemented these paths.

If any fail, the bug is in Task 3's `close_line`, not in these tests. Fix `close_line`. In particular, `TabAdvancesToTheNextTabStop` failing with zero diagnostics means the tab arithmetic used `+= TAB_STOP` instead of `(col / TAB_STOP + 1) * TAB_STOP`.

- [ ] **Step 3: Commit**

```bash
git add tests/domain/lexer/indentation_pass_test.cpp
git commit -m "Pin tab-stop arithmetic and TabError detection"
```

---

### Task 5: IndentationError, recovery, and the balance invariant

**Files:**
- Test: `tests/domain/lexer/indentation_pass_test.cpp` (append)

**Interfaces:**
- Consumes: everything from Tasks 3 and 4.
- Produces: nothing new.

The first two tests here are the regression tests for the stack-underflow bug the design review found. They must fail loudly (a crash, or unequal counts) against an implementation that overwrites the sentinel or pops unguarded.

- [ ] **Step 1: Write the failing tests**

Append to `tests/domain/lexer/indentation_pass_test.cpp`:

```cpp
TEST(IndentationPass, UnmatchedDedentReportsIndentationErrorAndContinues) {
    diagnostics::DiagnosticSink sink;
    const TypeList types = types_of("if a:\n    x\n  y\n", sink);

    ASSERT_EQ(sink.diagnostics().size(), 1u);
    EXPECT_EQ(sink.diagnostics().front().code, "IndentationError");
    EXPECT_EQ(sink.diagnostics().front().message, "unindent does not match any outer indentation level");
    EXPECT_EQ(sink.diagnostics().front().line, 3);
    // Scanning did not stop: the statement on the bad line is still in the
    // stream, so a later pass can keep reporting problems in this file.
    // Three identifiers -- the `a` in the header, then `x` and `y`.
    EXPECT_EQ(count_of(types, token_type::IDENTIFIER), 3);
}

TEST(IndentationPass, DedentingPastTheBaseLevelDoesNotUnbalanceTheStream) {
    // The regression case for sentinel overwriting. Line 3 dedents to column 2,
    // which matches no open level; if the base level were overwritten with 2,
    // line 4 at column 0 would pop it and emit a second DEDENT against a single
    // INDENT -- or pop an empty stack.
    diagnostics::DiagnosticSink sink;
    const TypeList types = types_of("if a:\n    x\n  y\nz\n", sink);

    EXPECT_EQ(count_of(types, token_type::INDENT), 1);
    EXPECT_EQ(count_of(types, token_type::DEDENT), 1);
    EXPECT_EQ(sink.diagnostics().size(), 1u);
}

TEST(IndentationPass, RepeatedDedentsPastTheBaseLevelStayBalanced) {
    diagnostics::DiagnosticSink sink;
    const TypeList types = types_of("if a:\n        x\n    y\n        z\n  w\n", sink);

    EXPECT_EQ(count_of(types, token_type::INDENT), 2);
    EXPECT_EQ(count_of(types, token_type::DEDENT), 2);
    EXPECT_EQ(sink.diagnostics().size(), 2u);
}

TEST(IndentationPass, DedentToAColumnBetweenTwoOpenLevelsAdoptsThatColumn) {
    // Column 6 sits between the open levels 4 and 8. One DEDENT is emitted for
    // level 8, then level 4 is overwritten with 6 -- a width change, not a
    // depth change, so the flush at end of file still balances.
    diagnostics::DiagnosticSink sink;
    const TypeList types = types_of("if a:\n    x\n        y\n      z\n", sink);

    EXPECT_EQ(count_of(types, token_type::INDENT), 2);
    EXPECT_EQ(count_of(types, token_type::DEDENT), 2);
    ASSERT_EQ(sink.diagnostics().size(), 1u);
    EXPECT_EQ(sink.diagnostics().front().code, "IndentationError");
}

TEST(IndentationPass, StreamWithoutAnEndOfFileTokenIsStillFlushed) {
    // Not something Lexer produces, but the balance guarantee is unconditional
    // and a hand-built stream must not be able to break it.
    std::vector<Token> tokens;
    tokens.emplace_back(token_type::SPACE, " ", 1, 1);
    tokens.emplace_back(token_type::SPACE, " ", 1, 2);
    tokens.emplace_back(token_type::IDENTIFIER, "x", 1, 3);

    diagnostics::DiagnosticSink sink;
    const TokenStream result = IndentationPass().run(TokenStream(std::move(tokens)), sink);

    TypeList types;
    for (const Token& token : result) {
        types.push_back(token.type());
    }
    EXPECT_EQ(types, (TypeList{token_type::INDENT, token_type::IDENTIFIER, token_type::DEDENT}));
}

TEST(IndentationPass, DeeplyNestedThenFullyDedentedFileStaysBalanced) {
    const TypeList types =
        types_of("if a:\n    if b:\n        if c:\n            if d:\n                x\ny\n");
    EXPECT_EQ(count_of(types, token_type::INDENT), 4);
    EXPECT_EQ(count_of(types, token_type::DEDENT), 4);
}
```

- [ ] **Step 2: Run the tests**

Run: `cmake --build build && ctest --test-dir build -R "IndentationPass" --output-on-failure`
Expected: all PASS.

A crash or a `std::vector` assertion in `DedentingPastTheBaseLevel...` means the pop loop in `close_line` is missing its `levels_.size() > 1` guard. Unequal INDENT/DEDENT counts mean the sentinel is being overwritten — the `if (levels_.size() > 1)` guard around `levels_.back() = line;` is missing.

- [ ] **Step 3: Commit**

```bash
git add tests/domain/lexer/indentation_pass_test.cpp
git commit -m "Pin IndentationError recovery and the INDENT/DEDENT balance invariant"
```

---

### Task 6: Lexer-interaction tests and the known deviations

**Files:**
- Test: `tests/domain/lexer/indentation_pass_test.cpp` (append)
- Modify: `src/domain/lexer/lexer.h:38-40` (the stale TODO)

**Interfaces:**
- Consumes: everything from Tasks 3-5.
- Produces: nothing new.

These are the cases where the pass's correctness depends on a subtlety of the lexer's output rather than on the algorithm. Each one is a place where a plausible implementation is wrong.

- [ ] **Step 1: Write the failing tests**

Append to `tests/domain/lexer/indentation_pass_test.cpp`:

```cpp
TEST(IndentationPass, BlankLinesBetweenStatementsDoNotChangeTheLevel) {
    const TypeList types = types_of("if a:\n    x\n\n\n    y\n");
    EXPECT_EQ(count_of(types, token_type::INDENT), 1);
    EXPECT_EQ(count_of(types, token_type::DEDENT), 1);
}

TEST(IndentationPass, CommentOnlyLineAtColumnZeroInsideABlockDoesNotDedent) {
    // The comment line emits a bare COMMENT_SINGLE with no whitespace and no
    // NEWLINE. Treating it as a logical line would read it as a dedent to
    // column 0 and close the block early.
    const TypeList types = types_of("if a:\n    x\n# note\n    y\n");
    EXPECT_EQ(count_of(types, token_type::INDENT), 1);
    EXPECT_EQ(count_of(types, token_type::DEDENT), 1);
    EXPECT_EQ(count_of(types, token_type::COMMENT_SINGLE), 1);
}

TEST(IndentationPass, DedentIsInsertedAfterACommentThatPrecedesTheDedentingLine) {
    // INDENT/DEDENT go immediately before the first *significant* token, so
    // trivia stays where the scanner put it.
    EXPECT_EQ(types_of("if a:\n    x\n# note\ny\n"),
              (TypeList{token_type::KEYWORD_IF, token_type::IDENTIFIER, token_type::COLON,
                        token_type::NEWLINE, token_type::INDENT, token_type::IDENTIFIER,
                        token_type::NEWLINE, token_type::COMMENT_SINGLE, token_type::DEDENT,
                        token_type::IDENTIFIER, token_type::NEWLINE, token_type::TOKEN_EOF}));
}

TEST(IndentationPass, DedentsReachEndOfFileEvenAfterATrailingCommentLine) {
    // The flush anchor is the TOKEN_EOF token, not "after the last NEWLINE".
    const TypeList types = types_of("if a:\n    x\n# note\n");
    ASSERT_GE(types.size(), 3u);
    EXPECT_EQ(types[types.size() - 1], token_type::TOKEN_EOF);
    EXPECT_EQ(types[types.size() - 2], token_type::DEDENT);
    EXPECT_EQ(types[types.size() - 3], token_type::COMMENT_SINGLE);
}

TEST(IndentationPass, ContinuationLineInsideBracketsIsNotMeasured) {
    // No NEWLINE and no whitespace tokens are emitted inside brackets, so the
    // whole call is one logical line at column 0.
    const TypeList types = types_of("f(a,\n        b)\n");
    EXPECT_EQ(count_of(types, token_type::INDENT), 0);
    EXPECT_EQ(count_of(types, token_type::DEDENT), 0);
}

TEST(IndentationPass, BackslashContinuationMeasuresTheFirstPhysicalLine) {
    // The whitespace run is on line 2 and the token it describes is on line 3,
    // so grouping a line by Token::line_number() instead of by adjacency would
    // lose the measurement entirely.
    diagnostics::DiagnosticSink sink;
    const TokenStream result = pass("if a:\n    \\\n    x\n", sink);

    ASSERT_GE(result.size(), 5u);
    const Token& indent = result.at(4);
    EXPECT_EQ(indent.type(), token_type::INDENT);
    EXPECT_EQ(indent.line_number(), 3);
    EXPECT_TRUE(sink.empty());
}

TEST(IndentationPass, SemicolonSeparatedStatementsAreOneLogicalLine) {
    const TypeList types = types_of("if a:\n    x = 1; y = 2\n");
    EXPECT_EQ(count_of(types, token_type::INDENT), 1);
    EXPECT_EQ(count_of(types, token_type::NEWLINE), 2);
}

TEST(IndentationPass, InlineBlockBodyProducesNoIndent) {
    const TypeList types = types_of("if a: x = 1\n");
    EXPECT_EQ(count_of(types, token_type::INDENT), 0);
}

TEST(IndentationPass, IndentationAfterAMultiLineStringIsMeasuredCorrectly) {
    // The scanner's line counter advances through the newlines inside the
    // string, so the run on the line after it carries the right position.
    const TypeList types = types_of("def f():\n    \"\"\"doc\n    more\n    \"\"\"\n    return 1\n");
    EXPECT_EQ(count_of(types, token_type::INDENT), 1);
    EXPECT_EQ(count_of(types, token_type::DEDENT), 1);
}

TEST(IndentationPass, UnterminatedBracketSuppressesIndentationWithoutUnbalancing) {
    const TypeList types = types_of("x = (\n    y\n");
    EXPECT_EQ(count_of(types, token_type::INDENT), 0);
    EXPECT_EQ(count_of(types, token_type::DEDENT), 0);
}

TEST(IndentationPass, UnterminatedTripleQuotedStringLeavesABalancedStream) {
    const TypeList types = types_of("if a:\n    \"\"\"oops\n    x\n");
    EXPECT_EQ(count_of(types, token_type::INDENT), count_of(types, token_type::DEDENT));
}

TEST(IndentationPass, CarriageReturnOnlyLineEndingsCollapseToOneLogicalLine) {
    // Known lexer limitation, pinned rather than worked around: a bare '\r' is
    // swallowed as inter-token whitespace and never re-arms the line start, so
    // no NEWLINE and no indentation tokens exist for such a file.
    const TypeList types = types_of("if x:\r    y\r");
    EXPECT_EQ(count_of(types, token_type::INDENT), 0);
    EXPECT_EQ(count_of(types, token_type::NEWLINE), 1);
}

TEST(IndentationPass, FormFeedBeforeIndentationDivergesFromCPython) {
    // Known deviation, pinned so it is visible rather than forgotten. The
    // scanner's emit loop accepts only ' ' and '\t' while the surrounding skip
    // loops also consume '\f', so the indentation after a leading form feed is
    // absent from the stream and no implementation of this pass can recover it.
    // CPython resets the column on a form feed and would measure column 4 here.
    const TypeList types = types_of("if a:\n\f    x\n");
    EXPECT_EQ(count_of(types, token_type::INDENT), 0);
}

TEST(IndentationPass, FormFeedAfterIndentationDivergesFromCPython) {
    // Same cause, opposite direction: the four spaces before the form feed are
    // counted, where CPython would count only the two after it.
    const TypeList types = types_of("if a:\n    \f  x\n");
    EXPECT_EQ(count_of(types, token_type::INDENT), 1);
}
```

- [ ] **Step 2: Run the tests**

Run: `cmake --build build && ctest --test-dir build -R "IndentationPass" --output-on-failure`
Expected: all PASS.

`DedentIsInsertedAfterAComment...` failing means `COMMENT_SINGLE` is resetting `measuring_` or being treated as significant. `BackslashContinuation...` failing means lines are being grouped by `line_number()`.

- [ ] **Step 3: Correct the stale TODO in the lexer**

In `src/domain/lexer/lexer.h`, replace lines 38-40:

```cpp
// TODO: INDENT/DEDENT tokens and TabError detection not yet implemented.
// When they land, blank and comment-only lines must be excluded from
// indentation processing, as CPython does.
```

with:

```cpp
// INDENT/DEDENT tokens and TabError detection are not produced here. They are
// IndentationPass's job, which consumes the SPACE/TAB tokens above; see
// indentation_pass.h. Blank and comment-only lines are already excluded from
// the whitespace this emits, as CPython does, so the pass never sees them.
```

- [ ] **Step 4: Run the whole suite**

Run: `cmake --build build && ctest --test-dir build --output-on-failure`
Expected: all PASS.

- [ ] **Step 5: Commit**

```bash
git add tests/domain/lexer/indentation_pass_test.cpp src/domain/lexer/lexer.h
git commit -m "Pin IndentationPass behaviour against real lexer output"
```

---

### Task 7: DiagnosticsReporter port and the console adapter

**Files:**
- Modify: `src/ports/diagnostics_reporter.h` (replace the whole interface)
- Create: `src/adapters/cli/console_diagnostics_reporter.h`
- Create: `src/adapters/cli/console_diagnostics_reporter.cpp`
- Test: `tests/adapters/cli/console_diagnostics_reporter_test.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `domain::diagnostics::Diagnostic` and `Severity` (Task 1).
- Produces: `ports::DiagnosticsReporter` with the pure virtual `void report(const std::string& path, const domain::diagnostics::Diagnostic& diagnostic)`; `adapters::cli::ConsoleDiagnosticsReporter` implementing it.

The port currently has no implementors, call sites, or tests anywhere in the repository, so replacing its signature breaks nothing.

- [ ] **Step 1: Write the failing test**

Create `tests/adapters/cli/console_diagnostics_reporter_test.cpp`:

```cpp
#include <gtest/gtest.h>

#include <iostream>
#include <sstream>
#include <streambuf>
#include <string>

#include "adapters/cli/console_diagnostics_reporter.h"

namespace cythonpp::adapters::cli {
namespace {

// Swaps std::cerr's buffer for the lifetime of the object, so a test can read
// back what the reporter wrote without the output polluting the test run.
class CapturedCerr {
public:
    CapturedCerr() : original_(std::cerr.rdbuf(captured_.rdbuf())) {}
    ~CapturedCerr() { std::cerr.rdbuf(original_); }

    std::string str() const { return captured_.str(); }

private:
    std::ostringstream captured_;
    std::streambuf* original_;
};

TEST(ConsoleDiagnosticsReporter, WritesPathLineColumnSeverityCodeAndMessage) {
    CapturedCerr captured;
    ConsoleDiagnosticsReporter reporter;
    reporter.report("pkg/a.py", domain::diagnostics::Diagnostic{domain::diagnostics::Severity::Error,
                                                                "TabError", "bad indentation", 7, 3});

    EXPECT_EQ(captured.str(), "pkg/a.py:7:3: error: TabError: bad indentation\n");
}

TEST(ConsoleDiagnosticsReporter, LabelsWarningsAsWarnings) {
    CapturedCerr captured;
    ConsoleDiagnosticsReporter reporter;
    reporter.report("a.py", domain::diagnostics::Diagnostic{domain::diagnostics::Severity::Warning,
                                                            "Advice", "reconsider", 1, 1});

    EXPECT_EQ(captured.str(), "a.py:1:1: warning: Advice: reconsider\n");
}

} // namespace
} // namespace cythonpp::adapters::cli
```

- [ ] **Step 2: Add the new files to the build**

In `CMakeLists.txt`, add to `CYTHONPP_LIB_SOURCES`:

```cmake
    src/adapters/cli/console_diagnostics_reporter.cpp
```

And to the `cythonpp_tests` source list:

```cmake
        tests/adapters/cli/console_diagnostics_reporter_test.cpp
```

- [ ] **Step 3: Run the test to verify it fails**

Run: `cmake -S . -B build -G Ninja && cmake --build build`
Expected: FAIL to compile — `adapters/cli/console_diagnostics_reporter.h` does not exist.

- [ ] **Step 4: Replace `src/ports/diagnostics_reporter.h`**

```cpp
#ifndef CYTHONPP_PORTS_DIAGNOSTICS_REPORTER_H
#define CYTHONPP_PORTS_DIAGNOSTICS_REPORTER_H

#include <string>

#include "domain/diagnostics/diagnostic.h"

namespace cythonpp::ports {

// Driven port: how the application layer reports problems back out.
//
// This is the one place ports/ depends on domain/. That is ordinary for a
// driven port -- it exists to speak the domain's language to the outside
// world -- and the alternative is flattening Diagnostic back into loose
// parameters at every call site, losing severity and code on the way.
class DiagnosticsReporter {
public:
    virtual ~DiagnosticsReporter() = default;

    // `path` is the source file the problem was found in. Diagnostic does not
    // carry it: a domain pass only ever sees one file's tokens and cannot know
    // its path, so the caller that does attaches it here.
    virtual void report(const std::string& path, const domain::diagnostics::Diagnostic& diagnostic) = 0;
};

} // namespace cythonpp::ports

#endif // CYTHONPP_PORTS_DIAGNOSTICS_REPORTER_H
```

- [ ] **Step 5: Write `src/adapters/cli/console_diagnostics_reporter.h`**

```cpp
#ifndef CYTHONPP_ADAPTERS_CLI_CONSOLE_DIAGNOSTICS_REPORTER_H
#define CYTHONPP_ADAPTERS_CLI_CONSOLE_DIAGNOSTICS_REPORTER_H

#include <string>

#include "ports/diagnostics_reporter.h"

namespace cythonpp::adapters::cli {

// Prints diagnostics to stderr in the shape clang and gcc use, so editors and
// build-tool problem matchers that already parse compiler output can jump
// straight to the offending line without configuration.
//
// stderr rather than stdout because stdout carries the token dump, and a user
// piping that to a file still wants to see the errors.
class ConsoleDiagnosticsReporter : public ports::DiagnosticsReporter {
public:
    void report(const std::string& path, const domain::diagnostics::Diagnostic& diagnostic) override;
};

} // namespace cythonpp::adapters::cli

#endif // CYTHONPP_ADAPTERS_CLI_CONSOLE_DIAGNOSTICS_REPORTER_H
```

- [ ] **Step 6: Write `src/adapters/cli/console_diagnostics_reporter.cpp`**

```cpp
#include "adapters/cli/console_diagnostics_reporter.h"

#include <iostream>

namespace cythonpp::adapters::cli {

void ConsoleDiagnosticsReporter::report(const std::string& path,
                                        const domain::diagnostics::Diagnostic& diagnostic) {
    const bool is_error = diagnostic.severity == domain::diagnostics::Severity::Error;
    std::cerr << path << ':' << diagnostic.line << ':' << diagnostic.column << ": "
              << (is_error ? "error" : "warning") << ": " << diagnostic.code << ": "
              << diagnostic.message << std::endl;
}

} // namespace cythonpp::adapters::cli
```

- [ ] **Step 7: Run the tests to verify they pass**

Run: `cmake --build build && ctest --test-dir build -R "ConsoleDiagnosticsReporter" --output-on-failure`
Expected: 2 tests, PASS.

- [ ] **Step 8: Commit**

```bash
git add CMakeLists.txt src/ports/diagnostics_reporter.h src/adapters/cli/console_diagnostics_reporter.h src/adapters/cli/console_diagnostics_reporter.cpp tests/adapters/cli/console_diagnostics_reporter_test.cpp
git commit -m "Give DiagnosticsReporter a Diagnostic-shaped signature and a console adapter"
```

---

### Task 8: Wire the pass into the pipeline and the CLI

**Files:**
- Modify: `src/application/compile_pipeline.h` (`CompileResult`, constructor, `compile_one`)
- Modify: `src/application/compile_pipeline.cpp`
- Modify: `src/adapters/cli/cli_adapter.cpp:64-86`
- Test: `tests/application/compile_pipeline_test.cpp` (add a fake, update 8 construction sites, add 4 tests)
- Modify: `CLAUDE.md`

**Interfaces:**
- Consumes: `IndentationPass` (Task 3), `DiagnosticSink` (Task 1), `ports::DiagnosticsReporter` and `ConsoleDiagnosticsReporter` (Task 7).
- Produces: `CompilePipeline(ports::SourceReader&, ports::SourceLister&, ports::DiagnosticsReporter&)`; `CompileResult` gains `bool has_errors = false`.

- [ ] **Step 1: Write the failing test**

In `tests/application/compile_pipeline_test.cpp`, add this fake after `FakeSourceLister`:

```cpp
class RecordingDiagnosticsReporter : public ports::DiagnosticsReporter {
public:
    struct Entry {
        std::string path;
        domain::diagnostics::Diagnostic diagnostic;
    };

    void report(const std::string& path, const domain::diagnostics::Diagnostic& diagnostic) override {
        entries.push_back(Entry{path, diagnostic});
    }

    std::vector<Entry> entries;
};
```

Add these includes at the top of the file:

```cpp
#include "domain/diagnostics/diagnostic.h"
#include "ports/diagnostics_reporter.h"
```

Update all eight `CompilePipeline pipeline(reader, lister);` construction sites in the file to declare a reporter and pass it:

```cpp
    RecordingDiagnosticsReporter reporter;
    CompilePipeline pipeline(reader, lister, reporter);
```

Then append these tests before the closing `} // namespace`:

```cpp
TEST(CompilePipeline, ModuleTokensHaveBeenThroughTheIndentationPass) {
    FakeSourceReader reader({{"a.py", "if x:\n    y\n"}});
    FakeSourceLister lister({});
    RecordingDiagnosticsReporter reporter;
    CompilePipeline pipeline(reader, lister, reporter);

    const CompileResult result = pipeline.compile_file("a.py");

    bool saw_indent = false;
    for (const domain::lexer::Token& token : result.modules.at("a.py")) {
        EXPECT_NE(token.type(), domain::lexer::token_type::SPACE);
        if (token.type() == domain::lexer::token_type::INDENT) {
            saw_indent = true;
        }
    }
    EXPECT_TRUE(saw_indent);
}

TEST(CompilePipeline, ACleanFileReportsNothingAndSetsNoErrorFlag) {
    FakeSourceReader reader({{"a.py", "if x:\n    y\n"}});
    FakeSourceLister lister({});
    RecordingDiagnosticsReporter reporter;
    CompilePipeline pipeline(reader, lister, reporter);

    const CompileResult result = pipeline.compile_file("a.py");

    EXPECT_TRUE(reporter.entries.empty());
    EXPECT_FALSE(result.has_errors);
}

TEST(CompilePipeline, IndentationDiagnosticsAreReportedAgainstTheirSourcePath) {
    FakeSourceReader reader({{"pkg/bad.py", "if a:\n\tx\n        y\n"}});
    FakeSourceLister lister({});
    RecordingDiagnosticsReporter reporter;
    CompilePipeline pipeline(reader, lister, reporter);

    const CompileResult result = pipeline.compile_file("pkg/bad.py");

    ASSERT_EQ(reporter.entries.size(), 1u);
    EXPECT_EQ(reporter.entries.front().path, "pkg/bad.py");
    EXPECT_EQ(reporter.entries.front().diagnostic.code, "TabError");
    EXPECT_TRUE(result.has_errors);
}

TEST(CompilePipeline, HasErrorsIsSetWhenAnyModuleInADirectoryFails) {
    FakeSourceReader reader({{"pkg/ok.py", "x = 1\n"}, {"pkg/bad.py", "if a:\n\tx\n        y\n"}});
    FakeSourceLister lister({"pkg/ok.py", "pkg/bad.py"});
    RecordingDiagnosticsReporter reporter;
    CompilePipeline pipeline(reader, lister, reporter);

    const CompileResult result = pipeline.compile_directory("pkg");

    EXPECT_EQ(result.modules.size(), 2u);
    EXPECT_TRUE(result.has_errors);
    EXPECT_EQ(reporter.entries.size(), 1u);
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build`
Expected: FAIL to compile — no matching constructor taking three arguments, and `CompileResult` has no member `has_errors`.

- [ ] **Step 3: Update `src/application/compile_pipeline.h`**

Add `#include "ports/diagnostics_reporter.h"` to the includes. Then:

```cpp
struct CompileResult {
    // Each source file's tokens, keyed by the path it was read from.
    // std::map rather than unordered_map so iteration order -- and with it
    // console output and test expectations -- is deterministic; at these
    // sizes hashing would buy nothing.
    std::map<std::string, domain::lexer::TokenStream> modules;

    // True if any module produced an error diagnostic. The diagnostics
    // themselves went to the reporter as each file was processed; this is only
    // what the CLI needs to pick an exit code, and a compiler that prints
    // errors and exits zero is a broken compiler.
    bool has_errors = false;
};
```

Change the constructor declaration to:

```cpp
    CompilePipeline(ports::SourceReader& source_reader,
                    ports::SourceLister& source_lister,
                    ports::DiagnosticsReporter& diagnostics_reporter);
```

Change the private section to:

```cpp
private:
    // Writes into `result` rather than returning a stream, because a module
    // contributes two things -- its tokens and whether it failed -- and
    // threading the second one back through a return value means an out
    // parameter either way.
    void compile_one(const std::string& path, CompileResult& result);

    ports::SourceReader& source_reader_;
    ports::SourceLister& source_lister_;
    ports::DiagnosticsReporter& diagnostics_reporter_;
```

- [ ] **Step 4: Update `src/application/compile_pipeline.cpp`**

```cpp
#include "application/compile_pipeline.h"

#include <utility>

#include "domain/diagnostics/diagnostic_sink.h"
#include "domain/lexer/indentation_pass.h"
#include "domain/lexer/lexer.h"

namespace cythonpp::application {

CompilePipeline::CompilePipeline(ports::SourceReader& source_reader,
                                 ports::SourceLister& source_lister,
                                 ports::DiagnosticsReporter& diagnostics_reporter)
    : source_reader_(source_reader),
      source_lister_(source_lister),
      diagnostics_reporter_(diagnostics_reporter) {}

void CompilePipeline::compile_one(const std::string& path, CompileResult& result) {
    domain::lexer::Lexer lexer(source_reader_.read(path));
    const domain::lexer::TokenStream lexed(lexer.tokenize());

    domain::diagnostics::DiagnosticSink sink;
    domain::lexer::TokenStream tokens = domain::lexer::IndentationPass().run(lexed, sink);

    // Reported per file as it is processed rather than buffered into the
    // result: on a directory run the user wants the first file's errors before
    // the last file has even been read.
    for (const domain::diagnostics::Diagnostic& diagnostic : sink.diagnostics()) {
        diagnostics_reporter_.report(path, diagnostic);
    }
    result.has_errors = result.has_errors || sink.has_errors();

    // TODO: parser / semantic analysis / codegen stages once implemented.
    result.modules.emplace(path, std::move(tokens));
}

CompileResult CompilePipeline::compile_file(const std::string& path) {
    CompileResult result;
    compile_one(path, result);
    return result;
}

CompileResult CompilePipeline::compile_directory(const std::string& directory) {
    CompileResult result;
    // A read failure aborts the whole run rather than being collected: it is a
    // problem with the invocation, not with the source, so it is not the kind
    // of thing the diagnostics sink is for.
    for (const std::string& path : source_lister_.list(directory)) {
        compile_one(path, result);
    }
    return result;
}

} // namespace cythonpp::application
```

- [ ] **Step 5: Update `src/adapters/cli/cli_adapter.cpp`**

Add the include:

```cpp
#include "adapters/cli/console_diagnostics_reporter.h"
```

Then change the body of `run` from `std::string path = argv[1];` through the end:

```cpp
    std::string path = argv[1];
    filesystem::FilesystemSourceReader source_reader;
    filesystem::FilesystemSourceLister source_lister;
    ConsoleDiagnosticsReporter diagnostics_reporter;
    application::CompilePipeline pipeline(source_reader, source_lister, diagnostics_reporter);

    try {
        // Interpreting argv is this adapter's job. Asking the OS whether a
        // path is a directory needs <filesystem>, which the application
        // layer must not depend on -- so the choice is made here, at the
        // composition root, and the pipeline just exposes two verbs.
        const application::CompileResult result = std::filesystem::is_directory(path)
                                                      ? pipeline.compile_directory(path)
                                                      : pipeline.compile_file(path);
        print_result(result);

        // The dump is still printed for a file with errors -- seeing the
        // tokens is exactly what helps when diagnosing one -- but the exit
        // code has to say the compile failed.
        return result.has_errors ? 1 : 0;
    } catch (const std::exception& error) {
        // Wider than runtime_error: <filesystem> throws filesystem_error and
        // TokenStream::at throws out_of_range, both worth reporting rather
        // than terminating on.
        std::cerr << error.what() << std::endl;
        return 1;
    }
}
```

Note the trailing `return 0;` at the end of the old function body is now unreachable and must be deleted.

- [ ] **Step 6: Run the whole suite**

Run: `cmake --build build && ctest --test-dir build --output-on-failure`
Expected: all PASS, including the eight pre-existing `CompilePipeline.*` tests. Their fixtures are all unindented one-liners, so no `INDENT`/`DEDENT` is produced and their token expectations still hold; `DEDENT`s are inserted *before* `TOKEN_EOF`, so the trailing-token assertions hold too.

- [ ] **Step 7: Update `CLAUDE.md`**

Four edits, all now stale:

1. In the **Project** paragraph, replace "the lexer is implemented (token model, scanner, and a directory-walking pipeline)" with "the lexer and indentation pass are implemented (token model, scanner, INDENT/DEDENT recovery, and a directory-walking pipeline)".
2. In the `domain/` bullet, add `indentation_pass.h/.cpp` to the `lexer/` file list, and add a new bullet: "`diagnostics/` — `diagnostic.h` (`Severity`, `Diagnostic`), `diagnostic_sink.h/.cpp` (an in-memory collector; concrete rather than an interface, because domain code must not depend on `ports/`)."
3. In the `ports/` bullet, replace "(the last is a stub, not yet consumed anywhere)" with "`DiagnosticsReporter` is the one port that includes from `domain/` — it carries a `domain::diagnostics::Diagnostic`, which is ordinary for a driven port."
4. In the **Lexer behaviour** section, replace "`INDENT`/`DEDENT` are not implemented yet." with a sentence pointing at the pass: "`IndentationPass` consumes those `SPACE`/`TAB` runs and replaces them with `INDENT`/`DEDENT`, applying CPython's `col`/`altcol` rule (a tab advances to the next multiple of 8; a tab counts as one alt column) and reporting `TabError` / `IndentationError`. It is total and always emits as many `DEDENT`s as `INDENT`s. Known deviations from CPython, both rooted in the scanner: a leading form feed loses the indentation around it, and a bare `\r` line ending is not a line ending at all."

- [ ] **Step 8: Verify end to end against a real file**

```bash
printf 'def f(x: int) -> int:\n    if x:\n        return 1\n    return 0\n' > /tmp/ok.py
./build/cythonpp.exe /tmp/ok.py; echo "exit=$?"
```
Expected: the dump contains `INDENT` and `DEDENT` lines and no `SPACE` lines; `exit=0`.

```bash
printf 'if a:\n\tx = 1\n        y = 2\n' > /tmp/bad.py
./build/cythonpp.exe /tmp/bad.py; echo "exit=$?"
```
Expected: stderr shows `/tmp/bad.py:3:9: error: TabError: inconsistent use of tabs and spaces in indentation`; `exit=1`.

- [ ] **Step 9: Commit**

```bash
git add CLAUDE.md src/application src/adapters/cli/cli_adapter.cpp tests/application/compile_pipeline_test.cpp
git commit -m "Wire IndentationPass and diagnostics through the pipeline and CLI"
```

---

## Verification

Full check, from a clean configure:

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

Everything must pass, including every pre-existing lexer test — `lexer.cpp` is not modified by this plan, so any lexer test failure means something went wrong.

Targeted runs while working:

```sh
ctest --test-dir build -R "IndentationPass" --output-on-failure
ctest --test-dir build -R "DiagnosticSink|ConsoleDiagnosticsReporter" --output-on-failure
ctest --test-dir build -R "CompilePipeline" --output-on-failure
ctest --test-dir build -R "TokenType|TokenCategory" --output-on-failure
```

End-to-end, on the binary:

```sh
./build/cythonpp.exe path/to/some/python/dir
```

The dump must contain no `SPACE` or `TAB` rows for any file, `INDENT` and `DEDENT` counts must match within each file, and the exit code must be 1 if and only if a diagnostic was printed.

## Follow-on work, deliberately not in this plan

- **Spec 2: AST node model** (`domain/ast/`) — hierarchy, visitor, source spans.
- **Spec 3: Expression parser** — precedence climbing over the `OPERATOR` category, soft-keyword re-classification of `match`/`case`/`_`, and an open-delimiter stack used solely so an unclosed bracket can be reported against the line that opened it.
- **Spec 4: Statement parser** — compound statements over `INDENT`/`DEDENT`, annotations, `def`/`class`, imports, producing a `Module`. This is where "unexpected indent" belongs: the pass emits a structurally valid `INDENT` for any deeper line, and only the grammar knows whether one was allowed there.
- **Spec 5: Symbol table and name resolution** (`domain/semantic/`) — scopes, imports, forward references, and the use-before-definition checks that motivated this work. Deferred because Python permits forward references, so no single left-to-right pass can decide them.
- **Lexer fixes** — form feed column handling and bare-`\r` line endings, both pinned by tests in Task 6.
