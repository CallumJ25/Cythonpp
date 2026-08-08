#ifndef CYTHONPP_APPLICATION_COMPILE_PIPELINE_H
#define CYTHONPP_APPLICATION_COMPILE_PIPELINE_H

#include <map>
#include <string>

#include "domain/lexer/token_stream.h"
#include "ports/source_lister.h"
#include "ports/source_reader.h"

namespace cythonpp::application {

struct CompileResult {
    // Each source file's tokens, keyed by the path it was read from.
    // std::map rather than unordered_map so iteration order -- and with it
    // console output and test expectations -- is deterministic; at these
    // sizes hashing would buy nothing.
    std::map<std::string, domain::lexer::TokenStream> modules;
};

// Orchestrates the compiler pipeline stages. Depends only on port
// interfaces, so it never learns whether source comes from a directory on
// disk, an archive, or a test fixture. Currently wires the lexer stage only;
// parser, semantic analysis, and codegen stages are TODO.
class CompilePipeline {
public:
    CompilePipeline(ports::SourceReader& source_reader, ports::SourceLister& source_lister);

    // Lexes a single file. The result holds exactly one entry, keyed by
    // `path`, so callers need only one result-handling path.
    CompileResult compile_file(const std::string& path);

    // Lexes every source file the lister reports under `directory`. Files
    // are lexed independently of one another -- cross-file dependency
    // resolution is not yet done.
    CompileResult compile_directory(const std::string& directory);

private:
    domain::lexer::TokenStream compile_one(const std::string& path);

    ports::SourceReader& source_reader_;
    ports::SourceLister& source_lister_;
};

} // namespace cythonpp::application

#endif // CYTHONPP_APPLICATION_COMPILE_PIPELINE_H
