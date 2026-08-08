#include "adapters/cli/cli_adapter.h"

int main(int argc, char** argv) {
    cythonpp::adapters::cli::CliAdapter cli;
    return cli.run(argc, argv);
}
