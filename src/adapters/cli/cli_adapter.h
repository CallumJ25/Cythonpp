#ifndef CYTHONPP_ADAPTERS_CLI_CLI_ADAPTER_H
#define CYTHONPP_ADAPTERS_CLI_CLI_ADAPTER_H

namespace cythonpp::adapters::cli {

// Driving (primary) adapter: parses argv and drives the application layer.
class CliAdapter {
public:
    int run(int argc, char** argv);
};

} // namespace cythonpp::adapters::cli

#endif // CYTHONPP_ADAPTERS_CLI_CLI_ADAPTER_H
