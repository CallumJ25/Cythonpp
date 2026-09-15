#include <gtest/gtest.h>

#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "application/codegen_mode.h"
#include "application/compile_pipeline.h"
#include "domain/ast/ast_printer.h"
#include "domain/ast/module.h"
#include "domain/diagnostics/diagnostic.h"
#include "domain/lexer/token_type.h"
#include "ports/diagnostics_reporter.h"

namespace cythonpp::application {
namespace {

class FakeSourceReader : public ports::SourceReader {
public:
    explicit FakeSourceReader(std::map<std::string, std::string> files) : files_(std::move(files)) {}

    std::string read(const std::string& path) const override {
        // find() rather than operator[], which does not compile on a const map.
        const auto found = files_.find(path);
        if (found == files_.end()) {
            throw std::runtime_error("could not open source file: " + path);
        }
        return found->second;
    }

private:
    std::map<std::string, std::string> files_;
};

class FakeSourceLister : public ports::SourceLister {
public:
    explicit FakeSourceLister(std::vector<std::string> paths) : paths_(std::move(paths)) {}

    std::vector<std::string> list(const std::string&) const override { return paths_; }

private:
    std::vector<std::string> paths_;
};

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

TEST(CompilePipeline, CompileFileProducesASingleModuleKeyedByItsPath) {
    FakeSourceReader reader({{"a.py", "x = 1\n"}});
    FakeSourceLister lister({});
    RecordingDiagnosticsReporter reporter;
    CompilePipeline pipeline(reader, lister, reporter);

    const CompileResult result = pipeline.compile_file("a.py", CodegenMode::Skip);

    ASSERT_EQ(result.modules.size(), 1u);
    ASSERT_EQ(result.modules.count("a.py"), 1u);
    EXPECT_FALSE(result.modules.at("a.py").tokens.empty());
}

TEST(CompilePipeline, CompileFilePropagatesRuntimeErrorForAnUnreadableFile) {
    FakeSourceReader reader({});
    FakeSourceLister lister({});
    RecordingDiagnosticsReporter reporter;
    CompilePipeline pipeline(reader, lister, reporter);

    EXPECT_THROW(pipeline.compile_file("missing.py", CodegenMode::Skip), std::runtime_error);
}

TEST(CompilePipeline, CompileDirectoryProducesOneModulePerListedFile) {
    FakeSourceReader reader({{"pkg/a.py", "x = 1\n"}, {"pkg/b.py", "y = 2\n"}, {"pkg/c.py", "z = 3\n"}});
    FakeSourceLister lister({"pkg/a.py", "pkg/b.py", "pkg/c.py"});
    RecordingDiagnosticsReporter reporter;
    CompilePipeline pipeline(reader, lister, reporter);

    const CompileResult result = pipeline.compile_directory("pkg", CodegenMode::Skip);

    ASSERT_EQ(result.modules.size(), 3u);
    EXPECT_EQ(result.modules.count("pkg/a.py"), 1u);
    EXPECT_EQ(result.modules.count("pkg/b.py"), 1u);
    EXPECT_EQ(result.modules.count("pkg/c.py"), 1u);
}

TEST(CompilePipeline, EachModuleHoldsItsOwnTokens) {
    FakeSourceReader reader({{"a.py", "x\n"}, {"b.py", "y = 1 + 2\n"}});
    FakeSourceLister lister({"a.py", "b.py"});
    RecordingDiagnosticsReporter reporter;
    CompilePipeline pipeline(reader, lister, reporter);

    const CompileResult result = pipeline.compile_directory("", CodegenMode::Skip);

    EXPECT_EQ(result.modules.at("a.py").tokens.at(0).lexeme(), "x");
    EXPECT_EQ(result.modules.at("b.py").tokens.at(0).lexeme(), "y");
    EXPECT_NE(result.modules.at("a.py").tokens.size(), result.modules.at("b.py").tokens.size());
}

TEST(CompilePipeline, ModuleKeysAreSortedRegardlessOfListerOrder) {
    FakeSourceReader reader({{"c.py", "x\n"}, {"a.py", "x\n"}, {"b.py", "x\n"}});
    FakeSourceLister lister({"c.py", "a.py", "b.py"});
    RecordingDiagnosticsReporter reporter;
    CompilePipeline pipeline(reader, lister, reporter);

    const CompileResult result = pipeline.compile_directory("", CodegenMode::Skip);

    std::vector<std::string> keys;
    for (const auto& module : result.modules) {
        keys.push_back(module.first);
    }
    EXPECT_EQ(keys, (std::vector<std::string>{"a.py", "b.py", "c.py"}));
}

TEST(CompilePipeline, CompileDirectoryWithNoSourceFilesProducesNoModules) {
    FakeSourceReader reader({});
    FakeSourceLister lister({});
    RecordingDiagnosticsReporter reporter;
    CompilePipeline pipeline(reader, lister, reporter);

    EXPECT_TRUE(pipeline.compile_directory("empty", CodegenMode::Skip).modules.empty());
}

TEST(CompilePipeline, CompileDirectoryPropagatesRuntimeErrorFromAnyFile) {
    FakeSourceReader reader({{"a.py", "x\n"}});
    FakeSourceLister lister({"a.py", "gone.py"});
    RecordingDiagnosticsReporter reporter;
    CompilePipeline pipeline(reader, lister, reporter);

    EXPECT_THROW(pipeline.compile_directory("pkg", CodegenMode::Skip), std::runtime_error);
}

TEST(CompilePipeline, EveryModuleEndsWithAnEndOfFileToken) {
    FakeSourceReader reader({{"a.py", "x = 1\n"}, {"b.py", ""}});
    FakeSourceLister lister({"a.py", "b.py"});
    RecordingDiagnosticsReporter reporter;
    CompilePipeline pipeline(reader, lister, reporter);

    const CompileResult result = pipeline.compile_directory("", CodegenMode::Skip);

    for (const auto& module : result.modules) {
        ASSERT_FALSE(module.second.tokens.empty()) << module.first;
        EXPECT_EQ(module.second.tokens.at(module.second.tokens.size() - 1).type(),
                  domain::lexer::token_type::TOKEN_EOF)
            << module.first;
    }
}

TEST(CompilePipeline, ModuleTokensHaveBeenThroughTheIndentationPass) {
    FakeSourceReader reader({{"a.py", "if x:\n    y\n"}});
    FakeSourceLister lister({});
    RecordingDiagnosticsReporter reporter;
    CompilePipeline pipeline(reader, lister, reporter);

    const CompileResult result = pipeline.compile_file("a.py", CodegenMode::Skip);

    bool saw_indent = false;
    for (const domain::lexer::Token& token : result.modules.at("a.py").tokens) {
        EXPECT_NE(token.type(), domain::lexer::token_type::SPACE);
        if (token.type() == domain::lexer::token_type::INDENT) {
            saw_indent = true;
        }
    }
    EXPECT_TRUE(saw_indent);
}

TEST(CompilePipeline, ACleanFileReportsNothingAndSetsNoErrorFlag) {
    // Was "if x:\n    y\n" before semantic analysis was wired in; x and y were
    // never bound, so once the type checker runs they are legitimate
    // NameErrors, not a clean file. Literals sidestep binding entirely while
    // still exercising the indented-block parse this test cares about.
    FakeSourceReader reader({{"a.py", "if 1:\n    2\n"}});
    FakeSourceLister lister({});
    RecordingDiagnosticsReporter reporter;
    CompilePipeline pipeline(reader, lister, reporter);

    const CompileResult result = pipeline.compile_file("a.py", CodegenMode::Skip);

    EXPECT_TRUE(reporter.entries.empty());
    EXPECT_FALSE(result.has_errors);
}

TEST(CompilePipeline, IndentationDiagnosticsAreReportedAgainstTheirSourcePath) {
    FakeSourceReader reader({{"pkg/bad.py", "if a:\n\tx\n        y\n"}});
    FakeSourceLister lister({});
    RecordingDiagnosticsReporter reporter;
    CompilePipeline pipeline(reader, lister, reporter);

    const CompileResult result = pipeline.compile_file("pkg/bad.py", CodegenMode::Skip);

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

    const CompileResult result = pipeline.compile_directory("pkg", CodegenMode::Skip);

    EXPECT_EQ(result.modules.size(), 2u);
    EXPECT_TRUE(result.has_errors);
    EXPECT_EQ(reporter.entries.size(), 1u);
}

TEST(CompilePipeline, EachModuleCarriesAParsedAst) {
    FakeSourceReader reader({{"a.py", "x = 1\n"}});
    FakeSourceLister lister({});
    RecordingDiagnosticsReporter reporter;
    CompilePipeline pipeline(reader, lister, reporter);

    const CompileResult result = pipeline.compile_file("a.py", CodegenMode::Skip);

    ASSERT_EQ(result.modules.count("a.py"), 1u);
    const CompiledModule& compiled = result.modules.at("a.py");
    ASSERT_NE(compiled.ast, nullptr);
    EXPECT_EQ(domain::ast::AstPrinter().print(*compiled.ast),
              "(Module\n  (Assign (Name x) (Constant 1)))");
    EXPECT_FALSE(compiled.tokens.empty());
}

TEST(CompilePipeline, ParserDiagnosticsAreReportedAgainstTheirSourcePath) {
    FakeSourceReader reader({{"pkg/bad.py", "import os\n"}});
    FakeSourceLister lister({});
    RecordingDiagnosticsReporter reporter;
    CompilePipeline pipeline(reader, lister, reporter);

    const CompileResult result = pipeline.compile_file("pkg/bad.py", CodegenMode::Skip);

    ASSERT_EQ(reporter.entries.size(), 1u);
    EXPECT_EQ(reporter.entries.front().path, "pkg/bad.py");
    EXPECT_EQ(reporter.entries.front().diagnostic.code, "SyntaxError");
    EXPECT_EQ(reporter.entries.front().diagnostic.message,
              "import statements are not supported");
    EXPECT_TRUE(result.has_errors);
}

TEST(CompilePipeline, AFileWithIndentationErrorsIsStillParsed) {
    // IndentationPass is total and balanced, so the stream is always
    // parseable. Running the parser anyway yields more diagnostics per
    // invocation than stopping at the first failing stage.
    FakeSourceReader reader({{"a.py", "if a:\n\tx = 1\n        y = 2\n"}});
    FakeSourceLister lister({});
    RecordingDiagnosticsReporter reporter;
    CompilePipeline pipeline(reader, lister, reporter);

    const CompileResult result = pipeline.compile_file("a.py", CodegenMode::Skip);

    ASSERT_NE(result.modules.at("a.py").ast, nullptr);
    EXPECT_TRUE(result.has_errors);
}

TEST(CompilePipeline, TheAstIsNeverNullEvenForAFileThatFailedEntirely) {
    FakeSourceReader reader({{"a.py", "import os\n"}});
    FakeSourceLister lister({});
    RecordingDiagnosticsReporter reporter;
    CompilePipeline pipeline(reader, lister, reporter);

    const CompileResult result = pipeline.compile_file("a.py", CodegenMode::Skip);

    ASSERT_NE(result.modules.at("a.py").ast, nullptr);
    EXPECT_TRUE(result.modules.at("a.py").ast->body().empty());
    EXPECT_TRUE(result.has_errors);
}

// THE SEAM. If the drain were left above the semantic pass this would pass
// vacuously with zero reported diagnostics and an exit code of success.
TEST(CompilePipeline, ReportsSemanticDiagnosticsToTheReporter) {
    FakeSourceReader reader({{"a.py", "x: int = \"s\"\n"}});
    FakeSourceLister lister({});
    RecordingDiagnosticsReporter reporter;
    CompilePipeline pipeline(reader, lister, reporter);

    const CompileResult result = pipeline.compile_file("a.py", CodegenMode::Skip);

    ASSERT_EQ(reporter.entries.size(), 1u) << "the semantic diagnostic must reach the reporter";
    EXPECT_EQ(reporter.entries.front().path, "a.py");
    EXPECT_EQ(reporter.entries.front().diagnostic.code, "TypeError");
    EXPECT_TRUE(result.has_errors) << "and the exit code must say the compile failed";
}

TEST(CompilePipeline, PopulatesTheTypeMapForACleanFile) {
    FakeSourceReader reader({{"a.py", "x: int = 5\n"}});
    FakeSourceLister lister({});
    RecordingDiagnosticsReporter reporter;
    CompilePipeline pipeline(reader, lister, reporter);

    const CompileResult result = pipeline.compile_file("a.py", CodegenMode::Skip);

    EXPECT_FALSE(result.has_errors);
    EXPECT_TRUE(reporter.entries.empty());
    EXPECT_GT(result.modules.at("a.py").types.size(), 0u);
}

// Skipped when the sink already has errors: a dropped statement removes a
// binding, so running the checker would invent a NameError for every later
// use of it. The dropped `import os` removes the only binding of `os`, and
// `y: int = os` reads that exact name -- so with the guard off this would
// report the SyntaxError plus an invented `NameError: name 'os' is not
// defined`, and with the guard on it must report only the former.
TEST(CompilePipeline, SkipsSemanticAnalysisWhenParsingFailed) {
    FakeSourceReader reader({{"a.py", "import os\ny: int = os\n"}});
    FakeSourceLister lister({});
    RecordingDiagnosticsReporter reporter;
    CompilePipeline pipeline(reader, lister, reporter);

    const CompileResult result = pipeline.compile_file("a.py", CodegenMode::Skip);

    EXPECT_TRUE(result.has_errors);
    EXPECT_EQ(result.modules.at("a.py").types.size(), 0u) << "the map must be empty";
    // Exactly the one syntax error, and no invented NameError for `os`.
    ASSERT_EQ(reporter.entries.size(), 1u);
    EXPECT_EQ(reporter.entries.front().diagnostic.code, "SyntaxError");
}

TEST(CompilePipeline, ChecksEveryFileInADirectoryRun) {
    FakeSourceReader reader({{"a.py", "x: int = 5\n"}, {"b.py", "y: int = \"s\"\n"}});
    FakeSourceLister lister({"a.py", "b.py"});
    RecordingDiagnosticsReporter reporter;
    CompilePipeline pipeline(reader, lister, reporter);

    const CompileResult result = pipeline.compile_directory(".", CodegenMode::Skip);

    EXPECT_TRUE(result.has_errors);
    EXPECT_GT(result.modules.at("a.py").types.size(), 0u);
    // b.py has no syntax error, so the checker still runs on it despite the
    // TypeError -- the mismatched value is still typed for the TypeMap (see
    // TypeChecker::visit(AnnAssign)), so this must be non-empty too.
    EXPECT_GT(result.modules.at("b.py").types.size(), 0u);
    ASSERT_EQ(reporter.entries.size(), 1u);
    EXPECT_EQ(reporter.entries.front().path, "b.py");
    EXPECT_EQ(reporter.entries.front().diagnostic.code, "TypeError");
}

TEST(CompilePipeline, TheTokenStreamCursorIsRewoundForTheCaller) {
    // The parser reads through the cursor, so whoever holds the result must
    // get a stream positioned at the start rather than wherever it stopped.
    FakeSourceReader reader({{"a.py", "x = 1\n"}});
    FakeSourceLister lister({});
    RecordingDiagnosticsReporter reporter;
    CompilePipeline pipeline(reader, lister, reporter);

    const CompileResult result = pipeline.compile_file("a.py", CodegenMode::Skip);

    EXPECT_EQ(result.modules.at("a.py").tokens.position(), 0u);
}

// Emission must not happen unless asked for. A program with an unsupported
// construct is silent under Skip and reports under Emit -- which is the
// whole reason CodegenMode exists as an explicit, non-defaulted parameter.
TEST(CompilePipeline, SkipModeReportsNoCodegenDiagnostics) {
    // A list literal type-checks clean but is outside the emitter's slice,
    // so this fixture only exercises codegen's own refusal, never the type
    // checker's.
    FakeSourceReader reader({{"a.py", "xs: list[int] = []\n"}});
    FakeSourceLister lister({});
    RecordingDiagnosticsReporter reporter;
    CompilePipeline pipeline(reader, lister, reporter);

    const CompileResult result = pipeline.compile_file("a.py", CodegenMode::Skip);

    EXPECT_TRUE(reporter.entries.empty());
    EXPECT_FALSE(result.has_errors);
    EXPECT_FALSE(result.modules.at("a.py").cpp.has_value());
}

TEST(CompilePipeline, EmitModeReportsTheCodegenRefusal) {
    FakeSourceReader reader({{"a.py", "xs: list[int] = []\n"}});
    FakeSourceLister lister({});
    RecordingDiagnosticsReporter reporter;
    CompilePipeline pipeline(reader, lister, reporter);

    const CompileResult result = pipeline.compile_file("a.py", CodegenMode::Emit);

    ASSERT_EQ(reporter.entries.size(), 1u);
    EXPECT_EQ(reporter.entries.front().diagnostic.code, "NotImplementedError");
    EXPECT_TRUE(result.has_errors);
    EXPECT_FALSE(result.modules.at("a.py").cpp.has_value());
}

TEST(CompilePipeline, EmitModeProducesSourceForASupportedProgram) {
    FakeSourceReader reader({{"a.py", "print(1)\n"}});
    FakeSourceLister lister({});
    RecordingDiagnosticsReporter reporter;
    CompilePipeline pipeline(reader, lister, reporter);

    const CompileResult result = pipeline.compile_file("a.py", CodegenMode::Emit);

    EXPECT_TRUE(reporter.entries.empty());
    EXPECT_FALSE(result.has_errors);
    ASSERT_TRUE(result.modules.at("a.py").cpp.has_value());
    EXPECT_NE(result.modules.at("a.py").cpp->find("int main()"), std::string::npos);
}

} // namespace
} // namespace cythonpp::application
