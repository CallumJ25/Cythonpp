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
