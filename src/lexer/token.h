#include <cstdint>

enum class token_type : uint16_t {
    // Special Tokens
    TOKEN_EOF = 0,
    TOKEN_ERROR,

    // Identifiers and Literals
    IDENTIFIER,
    LITERAL_NUMBER,
    LITERAL_STRING,

    // Keywords
    KEYWORD_IF,
    KEYWORD_ELSE,
    KEYWORD_WHILE,
    KEYWORD_RETURN,

    // Operators
    OP_ASSIGN,       // =
    OP_PLUS,         // +
    OP_MINUS,        // -
    OP_EQUAL,        // ==
    OP_NOT_EQUAL,    // !=
    OP_LESS,         // <
    OP_GREATER,      // >
    OP_LESS_EQUAL,   // <=
    OP_GREATER_EQUAL,// >=

    // Specific Brackets / Delimiters
    OPEN_PAREN,      // (
    CLOSE_PAREN,     // )
    OPEN_BRACE,      // {
    CLOSE_BRACE,     // }
    OPEN_BRACKET,    // [
    CLOSE_BRACKET,   // ]

    // Logical Operators
    OP_AND,          // &&
    OP_OR,           // ||
    OP_NOT,          // !

    // Punctuation
    SEMICOLON,       // ;
    COMMA,           // ,
    DOT,             // .
    COLON,           // :
    QUESTION,        // ?
    QUOTE,           // '
    DOUBLE_QUOTE,    // "

    // Spacing
    NEWLINE,         // \n
    SPACE,           // ' '
    TAB              // \t
};