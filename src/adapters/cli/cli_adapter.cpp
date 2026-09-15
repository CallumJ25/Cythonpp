#include "adapters/cli/cli_adapter.h"

#include <exception>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <string>

#include "adapters/cli/console_diagnostics_reporter.h"
#include "adapters/filesystem/filesystem_output_writer.h"
#include "adapters/filesystem/filesystem_source_lister.h"
#include "adapters/filesystem/filesystem_source_reader.h"
#include "application/codegen_mode.h"
#include "application/compile_pipeline.h"
#include "domain/ast/ast_printer.h"
#include "domain/ast/module.h"
#include "domain/lexer/token_type_name.h"
#include "domain/semantic/typed_printer.h"

namespace cythonpp::adapters::cli {

namespace {

// Which of the three mutually-exclusive dumps the CLI should print. A third
// bool alongside dump_tokens would admit a meaningless fourth (tokens AND
// types) state, so this is an enum instead.
enum class OutputMode { Tokens, Tree, Types };

// Renders control characters visibly so a NEWLINE or TAB token does not
// wreck the column alignment of the dump. Presentation only, which is why it
// lives here rather than in the domain.
std::string escape_lexeme(const std::string& lexeme) {
    std::string escaped;
    escaped.reserve(lexeme.size());
    for (char c : lexeme) {
        switch (c) {
            case '\n': escaped += "\\n"; break;
            case '\t': escaped += "\\t"; break;
            case '\r': escaped += "\\r"; break;
            case '\\': escaped += "\\\\"; break;
            case '"': escaped += "\\\""; break;
            default: escaped += c; break;
        }
    }
    return escaped;
}

void print_module(const std::string& path, const domain::lexer::TokenStream& tokens) {
    std::cout << "=== " << path << " (" << tokens.size() << " tokens) ===" << std::endl;
    for (const domain::lexer::Token& token : tokens) {
        std::cout << std::setw(6) << token.line_number() << ':' << std::left << std::setw(4)
                  << token.column_number() << ' ' << std::setw(24)
                  << domain::lexer::token_type_name(token.type()) << " \""
                  << escape_lexeme(token.lexeme()) << '"' << std::right << std::endl;
    }
}

void print_tree(const std::string& path, const domain::ast::Module& module) {
    std::cout << "=== " << path << " ===" << std::endl;
    std::cout << domain::ast::AstPrinter().print(module) << std::endl;
}

void print_typed_tree(const std::string& path, const domain::ast::Module& module,
                       const domain::semantic::TypeMap& types) {
    std::cout << "=== " << path << " ===" << std::endl;
    std::cout << domain::semantic::TypedPrinter().print(module, types) << std::endl;
}

void print_result(const application::CompileResult& result, OutputMode mode) {
    std::size_t total = 0;
    for (const auto& module : result.modules) {
        switch (mode) {
            case OutputMode::Tokens:
                print_module(module.first, module.second.tokens);
                total += module.second.tokens.size();
                break;
            case OutputMode::Tree:
                print_tree(module.first, *module.second.ast);
                break;
            case OutputMode::Types:
                print_typed_tree(module.first, *module.second.ast, module.second.types);
                break;
        }
    }
    if (mode == OutputMode::Tokens) {
        std::cout << result.modules.size() << " files, " << total << " tokens" << std::endl;
    } else {
        std::cout << result.modules.size() << " files" << std::endl;
    }
}

// Writes every module's emitted source to disk, at its own input path with
// the extension replaced by ".cpp". Only called once the caller has already
// confirmed the whole run is error-free, so `module.cpp` is always engaged
// here -- CodegenMode::Skip and a refused emission both leave it nullopt, and
// both are handled upstream by never reaching this function at all.
void write_emitted_sources(const application::CompileResult& result) {
    filesystem::FilesystemOutputWriter output_writer;
    for (const auto& module : result.modules) {
        std::filesystem::path output_path(module.first);
        output_path.replace_extension(".cpp");
        output_writer.write(output_path.string(), *module.second.cpp);
    }
}

} // namespace

int CliAdapter::run(int argc, char** argv) {
    OutputMode mode = OutputMode::Tree;
    bool emit_cpp = false;
    std::string path;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--tokens") {
            mode = OutputMode::Tokens;
        } else if (argument == "--types") {
            mode = OutputMode::Types;
        } else if (argument == "--emit-cpp") {
            // A separate flag, not a fourth OutputMode: the three existing
            // modes all print a dump to stdout, while this one writes a
            // file, and it can combine with any of them.
            emit_cpp = true;
        } else if (path.empty()) {
            path = argument;
        } else {
            std::cerr << "unexpected argument: " << argument << std::endl;
            return 1;
        }
    }
    if (path.empty()) {
        std::cerr
            << "usage: cythonpp [--tokens|--types] [--emit-cpp] <path-to-python-file-or-directory>"
            << std::endl;
        return 1;
    }

    filesystem::FilesystemSourceReader source_reader;
    filesystem::FilesystemSourceLister source_lister;
    ConsoleDiagnosticsReporter diagnostics_reporter;
    application::CompilePipeline pipeline(source_reader, source_lister, diagnostics_reporter);

    // CodegenMode has no default (see codegen_mode.h) precisely so this
    // choice cannot be forgotten: Skip for every other invocation, since
    // running the emitter unconditionally would report codegen's own
    // NotImplementedError refusals for a program the user only asked to see
    // tokenized or typed, never to compile.
    const application::CodegenMode codegen =
        emit_cpp ? application::CodegenMode::Emit : application::CodegenMode::Skip;

    try {
        // Interpreting argv is this adapter's job. Asking the OS whether a
        // path is a directory needs <filesystem>, which the application
        // layer must not depend on -- so the choice is made here, at the
        // composition root, and the pipeline just exposes two verbs.
        const application::CompileResult result = std::filesystem::is_directory(path)
                                                      ? pipeline.compile_directory(path, codegen)
                                                      : pipeline.compile_file(path, codegen);
        print_result(result, mode);

        // Refusing is always acceptable; writing a .cpp that does not
        // compile never is -- so a file is written only when the WHOLE run
        // is error-free, never per-module, and never at all on failure.
        if (emit_cpp && !result.has_errors) {
            write_emitted_sources(result);
        }

        // The tree is still printed for a file with errors -- seeing the
        // output is exactly what helps when diagnosing one -- but the exit
        // code has to say the compile failed.
        return result.has_errors ? 1 : 0;
    } catch (const std::exception& error) {
        // Wider than runtime_error: <filesystem> throws filesystem_error and
        // TokenStream::at throws out_of_range, both worth reporting rather
        // than terminating on.
        std::cerr << error.what() << std::endl;
        return 1;
    }
}

} // namespace cythonpp::adapters::cli
