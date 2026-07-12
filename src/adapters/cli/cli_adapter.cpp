#include "adapters/cli/cli_adapter.h"

#include <iostream>
#include <stdexcept>

#include "adapters/filesystem/filesystem_source_reader.h"
#include "application/compile_pipeline.h"

namespace cythonpp::adapters::cli {

int CliAdapter::run(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: cythonpp <path-to-python-file>" << std::endl;
        return 1;
    }

    std::string path = argv[1];
    filesystem::FilesystemSourceReader source_reader;
    application::CompilePipeline pipeline(source_reader);

    try {
        application::CompileResult result = pipeline.compile(path);
        std::cout << result.source << std::endl;
    } catch (const std::runtime_error& error) {
        std::cerr << error.what() << std::endl;
        return 1;
    }

    return 0;
}

} // namespace cythonpp::adapters::cli
