#include "application/compile_pipeline.h"

#include <memory>
#include <utility>

#include "domain/diagnostics/diagnostic_sink.h"
#include "domain/lexer/indentation_pass.h"
#include "domain/lexer/lexer.h"
#include "domain/parser/statement_parser.h"

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

    // One sink for both stages, so diagnostics reach the reporter in the
    // order they were found rather than lexer-pass-first. The parse runs even
    // when the indentation pass reported: that pass is total and balanced by
    // contract, so the stream is always parseable, and running it anyway
    // yields more diagnostics per invocation.
    domain::diagnostics::DiagnosticSink sink;
    domain::lexer::TokenStream tokens = domain::lexer::IndentationPass().run(lexed, sink);

    std::unique_ptr<domain::ast::Module> module =
        domain::parser::StatementParser(tokens, sink).parse_module();
    // The parser read through the stream's cursor. Rewound so whoever holds
    // the CompiledModule gets a stream at the start, not wherever it stopped.
    tokens.rewind();

    // Reported per file as it is processed rather than buffered into the
    // result: on a directory run the user wants the first file's errors before
    // the last file has even been read.
    for (const domain::diagnostics::Diagnostic& diagnostic : sink.diagnostics()) {
        diagnostics_reporter_.report(path, diagnostic);
    }
    result.has_errors = result.has_errors || sink.has_errors();

    // TODO: semantic analysis / codegen stages once implemented.
    CompiledModule compiled;
    compiled.tokens = std::move(tokens);
    compiled.ast = std::move(module);
    result.modules.emplace(path, std::move(compiled));
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
