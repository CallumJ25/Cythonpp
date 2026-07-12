#ifndef CYTHONPP_DOMAIN_LEXER_LEXER_H
#define CYTHONPP_DOMAIN_LEXER_LEXER_H

#include <string>
#include <vector>

#include "token.h"

namespace cythonpp::domain::lexer {

class Lexer {
public:
    explicit Lexer(std::string source);

    // TODO: character-scanning tokenizer algorithm not yet implemented.
    std::vector<Token> tokenize();

private:
    std::string source_;
};

} // namespace cythonpp::domain::lexer

#endif // CYTHONPP_DOMAIN_LEXER_LEXER_H
