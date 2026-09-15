#include "application/compile_pipeline.h"

#include <memory>
#include <utility>

#include "domain/codegen/emitter.h"
#include "domain/diagnostics/diagnostic_sink.h"
#include "domain/lexer/indentation_pass.h"
#include "domain/lexer/lexer.h"
#include "domain/parser/statement_parser.h"
#include "domain/semantic/type_checker.h"
#include "domain/semantic/type_map.h"

namespace cythonpp::application {

CompilePipeline::CompilePipeline(ports::SourceReader& source_reader,
                                 ports::SourceLister& source_lister,
                                 ports::DiagnosticsReporter& diagnostics_reporter)
    : source_reader_(source_reader),
      source_lister_(source_lister),
      diagnostics_reporter_(diagnostics_reporter) {}

void CompilePipeline::compile_one(const std::string& path, CodegenMode codegen,
                                  CompileResult& result) {
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

    // Skipped when the sink already has errors, unlike the parse above. The
    // parse ran after an indentation error because IndentationPass is
    // balanced by contract and the stream was guaranteed parseable. There is
    // no equivalent guarantee here: StatementParser DROPS a failed statement,
    // and a dropped statement removes a BINDING, so every later use of it
    // becomes a spurious NameError. One honest syntax error beats one syntax
    // error plus twenty invented name errors.
    domain::semantic::TypeMap types;
    if (!sink.has_errors()) {
        types = domain::semantic::TypeChecker(sink).check(*module);
    }

    // Codegen runs only when explicitly requested AND the file is still
    // clean after the semantic pass -- the same gate the type checker itself
    // uses above, for the same reason: emitting from a module the checker
    // already rejected would either crash on missing type information or
    // silently produce a .cpp for a program that must not compile. Emitted
    // BEFORE the diagnostic drain below, not after -- see that drain's own
    // comment, which already records that a stage wired in below it once had
    // every diagnostic silently discarded while still exiting zero. A
    // codegen refusal is reported into this SAME sink, so it drains with
    // everything else in this one pass.
    std::optional<std::string> cpp;
    if (codegen == CodegenMode::Emit && !sink.has_errors()) {
        cpp = domain::codegen::Emitter(types, sink).emit_module(*module);
    }

    // Reported per file as it is processed rather than buffered into the
    // result: on a directory run the user wants the first file's errors before
    // the last file has even been read. Drained AFTER the semantic pass, not
    // before -- the TODO this replaced sat above this drain, so a stage wired
    // in there would have had every diagnostic silently discarded and still
    // exited zero.
    for (const domain::diagnostics::Diagnostic& diagnostic : sink.diagnostics()) {
        diagnostics_reporter_.report(path, diagnostic);
    }
    result.has_errors = result.has_errors || sink.has_errors();

    CompiledModule compiled;
    compiled.tokens = std::move(tokens);
    compiled.ast = std::move(module);
    compiled.types = std::move(types);
    compiled.cpp = std::move(cpp);
    result.modules.emplace(path, std::move(compiled));
}

CompileResult CompilePipeline::compile_file(const std::string& path, CodegenMode codegen) {
    CompileResult result;
    compile_one(path, codegen, result);
    return result;
}

CompileResult CompilePipeline::compile_directory(const std::string& directory, CodegenMode codegen) {
    CompileResult result;
    // A read failure aborts the whole run rather than being collected: it is a
    // problem with the invocation, not with the source, so it is not the kind
    // of thing the diagnostics sink is for.
    for (const std::string& path : source_lister_.list(directory)) {
        compile_one(path, codegen, result);
    }
    return result;
}

} // namespace cythonpp::application
