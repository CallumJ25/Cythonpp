#include "lexer.h"

namespace cythonpp::domain::lexer {

Lexer::Lexer(std::string source) : source_(std::move(source)) {}

std::vector<Token> Lexer::tokenize() {
    // TODO: scan source_ into a stream of Tokens. Not yet implemented.
    return {};
}

} // namespace cythonpp::domain::lexer
