#ifndef CYTHONPP_DOMAIN_LEXER_TOKEN_H
#define CYTHONPP_DOMAIN_LEXER_TOKEN_H

#include <string>

#include "token_type.h"

namespace cythonpp::domain::lexer {

class Token {
public:
    Token(token_type type, std::string lexeme, int line_number, int column_number,
          int end_line, int end_column);

    token_type type() const;
    const std::string& lexeme() const;
    int line_number() const;
    int column_number() const;

    // One past the last character, so a zero-width synthesized token has
    // end == start. Columns count UTF-8 characters, matching
    // Lexer::advance(); a token that ends the line ends at column 1 of the
    // next one.
    int end_line() const;
    int end_column() const;

private:
    token_type type_;
    std::string lexeme_;
    int line_number_;
    int column_number_;
    int end_line_;
    int end_column_;
};

} // namespace cythonpp::domain::lexer

#endif // CYTHONPP_DOMAIN_LEXER_TOKEN_H
