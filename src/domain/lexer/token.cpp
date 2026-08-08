#include "token.h"

namespace cythonpp::domain::lexer {

Token::Token(token_type type, std::string lexeme, int line_number, int column_number)
    : type_(type),
      lexeme_(std::move(lexeme)),
      line_number_(line_number),
      column_number_(column_number) {}

token_type Token::type() const { return type_; }

const std::string& Token::lexeme() const { return lexeme_; }

int Token::line_number() const { return line_number_; }

int Token::column_number() const { return column_number_; }

} // namespace cythonpp::domain::lexer
