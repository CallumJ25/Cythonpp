#ifndef CYTHONPP_APPLICATION_COMPILE_PIPELINE_H
#define CYTHONPP_APPLICATION_COMPILE_PIPELINE_H

#include <map>
#include <memory>
#include <string>

#include "domain/ast/module.h"
#include "domain/lexer/token_stream.h"
#include "ports/diagnostics_reporter.h"
#include "ports/source_lister.h"
#include "ports/source_reader.h"

namespace cythonpp::application {

// One source file's compilation artifacts. A struct rather than two parallel
// maps: two maps keyed by the same path drift, and a file present in one but
// absent from the other is a bug waiting to happen. Later stages -- a typed
// IR, emitted C++ -- hang off this rather than adding a third map.
//
// Move-only, because ast::Module is non-copyable. That propagates to
// CompileResult, which is returned by value; C++17's guaranteed copy elision
// keeps `const CompileResult r = pipeline.compile_file(p);` valid.
struct CompiledModule {
    domain::lexer::TokenStream tokens;

    // Never null. StatementParser::parse_module returns an empty Module for a
    // file whose every statement failed, rather than nothing at all.
    std::unique_ptr<domain::ast::Module> ast;
};

struct CompileResult {
    // Each source file's artifacts, keyed by the path it was read from.
    // std::map rather than unordered_map so iteration order -- and with it
    // console output and test expectations -- is deterministic; at these
    // sizes hashing would buy nothing.
    std::map<std::string, CompiledModule> modules;

    // True if any module produced an error diagnostic. The diagnostics
    // themselves went to the reporter as each file was processed; this is only
    // what the CLI needs to pick an exit code, and a compiler that prints
    // errors and exits zero is a broken compiler.
    bool has_errors = false;
};

// Orchestrates the compiler pipeline stages. Depends only on port
// interfaces, so it never learns whether source comes from a directory on
// disk, an archive, or a test fixture. Currently wires the lexer and parser
// stages; semantic analysis and codegen stages are TODO.
class CompilePipeline {
public:
    CompilePipeline(ports::SourceReader& source_reader,
                    ports::SourceLister& source_lister,
                    ports::DiagnosticsReporter& diagnostics_reporter);

    // Lexes and parses a single file. The result holds exactly one entry,
    // keyed by `path`, so callers need only one result-handling path.
    CompileResult compile_file(const std::string& path);

    // Lexes and parses every source file the lister reports under
    // `directory`. Files are processed independently of one another --
    // cross-file dependency resolution is not yet done.
    CompileResult compile_directory(const std::string& directory);

private:
    // Writes into `result` rather than returning a stream, because a module
    // contributes three things -- its tokens, its AST, and whether it failed
    // -- and threading the rest back through a return value means an out
    // parameter either way.
    void compile_one(const std::string& path, CompileResult& result);

    ports::SourceReader& source_reader_;
    ports::SourceLister& source_lister_;
    ports::DiagnosticsReporter& diagnostics_reporter_;
};

} // namespace cythonpp::application

#endif // CYTHONPP_APPLICATION_COMPILE_PIPELINE_H
