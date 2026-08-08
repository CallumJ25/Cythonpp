#include "application/compile_pipeline.h"

#include <utility>

#include "domain/lexer/lexer.h"

namespace cythonpp::application {

CompilePipeline::CompilePipeline(ports::SourceReader& source_reader, ports::SourceLister& source_lister)
    : source_reader_(source_reader), source_lister_(source_lister) {}

domain::lexer::TokenStream CompilePipeline::compile_one(const std::string& path) {
    domain::lexer::Lexer lexer(source_reader_.read(path));

    // TODO: parser / semantic analysis / codegen stages once implemented.
    return domain::lexer::TokenStream(lexer.tokenize());
}

CompileResult CompilePipeline::compile_file(const std::string& path) {
    CompileResult result;
    result.modules.emplace(path, compile_one(path));
    return result;
}

CompileResult CompilePipeline::compile_directory(const std::string& directory) {
    CompileResult result;
    // A read failure aborts the whole run rather than being collected: fail
    // fast while there is no diagnostics pass to collect into.
    for (const std::string& path : source_lister_.list(directory)) {
        result.modules.emplace(path, compile_one(path));
    }
    return result;
}

} // namespace cythonpp::application
