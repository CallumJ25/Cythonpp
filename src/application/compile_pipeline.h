#ifndef CYTHONPP_APPLICATION_COMPILE_PIPELINE_H
#define CYTHONPP_APPLICATION_COMPILE_PIPELINE_H

#include <string>
#include <vector>

#include "domain/lexer/token.h"
#include "ports/source_reader.h"

namespace cythonpp::application {

struct CompileResult {
    std::string source;
    std::vector<domain::lexer::Token> tokens;
};

// Orchestrates the compiler pipeline stages. Depends only on port
// interfaces so it never knows whether source comes from a file, a string,
// or a network socket. Currently wires the lexer stage only; parser,
// semantic analysis, and codegen stages are TODO.
class CompilePipeline {
public:
    explicit CompilePipeline(ports::SourceReader& source_reader);

    CompileResult compile(const std::string& path);

private:
    ports::SourceReader& source_reader_;
};

} // namespace cythonpp::application

#endif // CYTHONPP_APPLICATION_COMPILE_PIPELINE_H
