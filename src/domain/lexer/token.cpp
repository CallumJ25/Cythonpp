#include "token.h"

namespace cythonpp::domain::lexer {

Token::Token(token_type type, std::string lexeme, int line_number, int column_number,
             int end_line, int end_column)
    : type_(type),
      lexeme_(std::move(lexeme)),
      line_number_(line_number),
      column_number_(column_number),
      end_line_(end_line),
      end_column_(end_column) {}

token_type Token::type() const { return type_; }

const std::string& Token::lexeme() const { return lexeme_; }

int Token::line_number() const { return line_number_; }

int Token::column_number() const { return column_number_; }

int Token::end_line() const { return end_line_; }

int Token::end_column() const { return end_column_; }

} // namespace cythonpp::domain::lexer
