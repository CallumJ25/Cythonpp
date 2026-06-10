#include <string>

#include "token.h";

struct token {
    token_type id;
    std::string lexeme;
    int line_number;
    int column_number;
};