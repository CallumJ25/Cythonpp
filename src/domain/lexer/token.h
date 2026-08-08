#ifndef CYTHONPP_DOMAIN_LEXER_TOKEN_H
#define CYTHONPP_DOMAIN_LEXER_TOKEN_H

#include <string>

#include "token_type.h"

namespace cythonpp::domain::lexer {

class Token {
public:
    Token(token_type type, std::string lexeme, int line_number, int column_number);

    token_type type() const;
    const std::string& lexeme() const;
    int line_number() const;
    int column_number() const;

private:
    token_type type_;
    std::string lexeme_;
    int line_number_;
    int column_number_;
};

} // namespace cythonpp::domain::lexer

#endif // CYTHONPP_DOMAIN_LEXER_TOKEN_H
