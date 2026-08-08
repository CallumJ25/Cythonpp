#include <gtest/gtest.h>

#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "application/compile_pipeline.h"
#include "domain/lexer/token_type.h"

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

TEST(CompilePipeline, CompileFileProducesASingleModuleKeyedByItsPath) {
    FakeSourceReader reader({{"a.py", "x = 1\n"}});
    FakeSourceLister lister({});
    CompilePipeline pipeline(reader, lister);

    const CompileResult result = pipeline.compile_file("a.py");

    ASSERT_EQ(result.modules.size(), 1u);
    ASSERT_EQ(result.modules.count("a.py"), 1u);
    EXPECT_FALSE(result.modules.at("a.py").empty());
}

TEST(CompilePipeline, CompileFilePropagatesRuntimeErrorForAnUnreadableFile) {
    FakeSourceReader reader({});
    FakeSourceLister lister({});
    CompilePipeline pipeline(reader, lister);

    EXPECT_THROW(pipeline.compile_file("missing.py"), std::runtime_error);
}

TEST(CompilePipeline, CompileDirectoryProducesOneModulePerListedFile) {
    FakeSourceReader reader({{"pkg/a.py", "x = 1\n"}, {"pkg/b.py", "y = 2\n"}, {"pkg/c.py", "z = 3\n"}});
    FakeSourceLister lister({"pkg/a.py", "pkg/b.py", "pkg/c.py"});
    CompilePipeline pipeline(reader, lister);

    const CompileResult result = pipeline.compile_directory("pkg");

    ASSERT_EQ(result.modules.size(), 3u);
    EXPECT_EQ(result.modules.count("pkg/a.py"), 1u);
    EXPECT_EQ(result.modules.count("pkg/b.py"), 1u);
    EXPECT_EQ(result.modules.count("pkg/c.py"), 1u);
}

TEST(CompilePipeline, EachModuleHoldsItsOwnTokens) {
    FakeSourceReader reader({{"a.py", "x\n"}, {"b.py", "y = 1 + 2\n"}});
    FakeSourceLister lister({"a.py", "b.py"});
    CompilePipeline pipeline(reader, lister);

    const CompileResult result = pipeline.compile_directory("");

    EXPECT_EQ(result.modules.at("a.py").at(0).lexeme(), "x");
    EXPECT_EQ(result.modules.at("b.py").at(0).lexeme(), "y");
    EXPECT_NE(result.modules.at("a.py").size(), result.modules.at("b.py").size());
}

TEST(CompilePipeline, ModuleKeysAreSortedRegardlessOfListerOrder) {
    FakeSourceReader reader({{"c.py", "x\n"}, {"a.py", "x\n"}, {"b.py", "x\n"}});
    FakeSourceLister lister({"c.py", "a.py", "b.py"});
    CompilePipeline pipeline(reader, lister);

    const CompileResult result = pipeline.compile_directory("");

    std::vector<std::string> keys;
    for (const auto& module : result.modules) {
        keys.push_back(module.first);
    }
    EXPECT_EQ(keys, (std::vector<std::string>{"a.py", "b.py", "c.py"}));
}

TEST(CompilePipeline, CompileDirectoryWithNoSourceFilesProducesNoModules) {
    FakeSourceReader reader({});
    FakeSourceLister lister({});
    CompilePipeline pipeline(reader, lister);

    EXPECT_TRUE(pipeline.compile_directory("empty").modules.empty());
}

TEST(CompilePipeline, CompileDirectoryPropagatesRuntimeErrorFromAnyFile) {
    FakeSourceReader reader({{"a.py", "x\n"}});
    FakeSourceLister lister({"a.py", "gone.py"});
    CompilePipeline pipeline(reader, lister);

    EXPECT_THROW(pipeline.compile_directory("pkg"), std::runtime_error);
}

TEST(CompilePipeline, EveryModuleEndsWithAnEndOfFileToken) {
    FakeSourceReader reader({{"a.py", "x = 1\n"}, {"b.py", ""}});
    FakeSourceLister lister({"a.py", "b.py"});
    CompilePipeline pipeline(reader, lister);

    const CompileResult result = pipeline.compile_directory("");

    for (const auto& module : result.modules) {
        ASSERT_FALSE(module.second.empty()) << module.first;
        EXPECT_EQ(module.second.at(module.second.size() - 1).type(), domain::lexer::token_type::TOKEN_EOF)
            << module.first;
    }
}

} // namespace
} // namespace cythonpp::application
