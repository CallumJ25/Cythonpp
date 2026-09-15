#ifndef CYTHONPP_APPLICATION_CODEGEN_MODE_H
#define CYTHONPP_APPLICATION_CODEGEN_MODE_H

namespace cythonpp::application {

// Whether CompilePipeline should run the Emitter after a successful type
// check. An enum rather than a bool for the reason OutputMode
// (adapters/cli/cli_adapter.cpp) already is one: a bare `true` at a call site
// says nothing about what it selects, where `CodegenMode::Emit` does.
//
// NO DEFAULT VALUE, deliberately -- the same idiom as
// DiagnosticSink::report's Suppressibility parameter (see that header's own
// comment). A default is the thing a caller forgets, and forgetting here
// means every existing call site (--tokens, --types, the plain tree dump)
// would silently start running codegen and reporting its NotImplementedError
// refusals for a program the user never asked to compile. Adding this
// parameter is therefore meant to break every existing call site; each one
// must say explicitly whether it wants codegen, and CodegenMode::Skip is the
// answer for all of them except --emit-cpp.
enum class CodegenMode { Skip, Emit };

} // namespace cythonpp::application

#endif // CYTHONPP_APPLICATION_CODEGEN_MODE_H
