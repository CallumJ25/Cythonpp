#include "adapters/cli/cli_adapter.h"

#include <exception>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <string>

#include "adapters/filesystem/filesystem_source_lister.h"
#include "adapters/filesystem/filesystem_source_reader.h"
#include "application/compile_pipeline.h"
#include "domain/lexer/token_type_name.h"

namespace cythonpp::adapters::cli {

namespace {

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

void print_result(const application::CompileResult& result) {
    std::size_t total = 0;
    for (const auto& module : result.modules) {
        print_module(module.first, module.second);
        total += module.second.size();
    }
    std::cout << result.modules.size() << " files, " << total << " tokens" << std::endl;
}

} // namespace

int CliAdapter::run(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: cythonpp <path-to-python-file-or-directory>" << std::endl;
        return 1;
    }

    std::string path = argv[1];
    filesystem::FilesystemSourceReader source_reader;
    filesystem::FilesystemSourceLister source_lister;
    application::CompilePipeline pipeline(source_reader, source_lister);

    try {
        // Interpreting argv is this adapter's job. Asking the OS whether a
        // path is a directory needs <filesystem>, which the application
        // layer must not depend on -- so the choice is made here, at the
        // composition root, and the pipeline just exposes two verbs.
        const application::CompileResult result = std::filesystem::is_directory(path)
                                                      ? pipeline.compile_directory(path)
                                                      : pipeline.compile_file(path);
        print_result(result);
    } catch (const std::exception& error) {
        // Wider than runtime_error: <filesystem> throws filesystem_error and
        // TokenStream::at throws out_of_range, both worth reporting rather
        // than terminating on.
        std::cerr << error.what() << std::endl;
        return 1;
    }

    return 0;
}

} // namespace cythonpp::adapters::cli
