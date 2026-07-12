#include "application/compile_pipeline.h"

#include "domain/lexer/lexer.h"

namespace cythonpp::application {

CompilePipeline::CompilePipeline(ports::SourceReader& source_reader)
    : source_reader_(source_reader) {}

CompileResult CompilePipeline::compile(const std::string& path) {
    std::string source = source_reader_.read(path);

    domain::lexer::Lexer lexer(source);
    std::vector<domain::lexer::Token> tokens = lexer.tokenize();

    // TODO: parser / semantic analysis / codegen stages once implemented.

    return CompileResult{std::move(source), std::move(tokens)};
}

} // namespace cythonpp::application
