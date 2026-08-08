#include "token_type_name.h"

namespace cythonpp::domain::lexer {

// A switch over the enum rather than a table, so clang's -Wswitch reports a
// newly added enumerator that has no name here instead of it silently
// printing "UNKNOWN".
std::string_view token_type_name(token_type type) {
    switch (type) {
        case token_type::TOKEN_EOF: return "TOKEN_EOF";
        case token_type::TOKEN_ERROR: return "TOKEN_ERROR";
        case token_type::NEWLINE: return "NEWLINE";
        case token_type::SPACE: return "SPACE";
        case token_type::TAB: return "TAB";

        case token_type::IDENTIFIER: return "IDENTIFIER";

        case token_type::LITERAL_STRING: return "LITERAL_STRING";
        case token_type::LITERAL_BYTES: return "LITERAL_BYTES";
        case token_type::FSTRING_START: return "FSTRING_START";
        case token_type::FSTRING_MIDDLE: return "FSTRING_MIDDLE";
        case token_type::FSTRING_END: return "FSTRING_END";

        case token_type::LITERAL_INT: return "LITERAL_INT";
        case token_type::LITERAL_FLOAT: return "LITERAL_FLOAT";
        case token_type::LITERAL_COMPLEX: return "LITERAL_COMPLEX";

        case token_type::BOOL_TRUE: return "BOOL_TRUE";
        case token_type::BOOL_FALSE: return "BOOL_FALSE";
        case token_type::KEYWORD_NONE: return "KEYWORD_NONE";
        case token_type::ELLIPSIS: return "ELLIPSIS";

        case token_type::KEYWORD_IF: return "KEYWORD_IF";
        case token_type::KEYWORD_ELSE: return "KEYWORD_ELSE";
        case token_type::KEYWORD_FOR: return "KEYWORD_FOR";
        case token_type::KEYWORD_ELIF: return "KEYWORD_ELIF";
        case token_type::KEYWORD_WHILE: return "KEYWORD_WHILE";
        case token_type::KEYWORD_RETURN: return "KEYWORD_RETURN";
        case token_type::KEYWORD_DEF: return "KEYWORD_DEF";
        case token_type::KEYWORD_CLASS: return "KEYWORD_CLASS";
        case token_type::KEYWORD_AS: return "KEYWORD_AS";
        case token_type::KEYWORD_ASSERT: return "KEYWORD_ASSERT";
        case token_type::KEYWORD_ASYNC: return "KEYWORD_ASYNC";
        case token_type::KEYWORD_AWAIT: return "KEYWORD_AWAIT";
        case token_type::KEYWORD_BREAK: return "KEYWORD_BREAK";
        case token_type::KEYWORD_CONTINUE: return "KEYWORD_CONTINUE";
        case token_type::KEYWORD_DEL: return "KEYWORD_DEL";
        case token_type::KEYWORD_EXCEPT: return "KEYWORD_EXCEPT";
        case token_type::KEYWORD_FINALLY: return "KEYWORD_FINALLY";
        case token_type::KEYWORD_FROM: return "KEYWORD_FROM";
        case token_type::KEYWORD_GLOBAL: return "KEYWORD_GLOBAL";
        case token_type::KEYWORD_IMPORT: return "KEYWORD_IMPORT";
        case token_type::KEYWORD_LAMBDA: return "KEYWORD_LAMBDA";
        case token_type::KEYWORD_NONLOCAL: return "KEYWORD_NONLOCAL";
        case token_type::KEYWORD_PASS: return "KEYWORD_PASS";
        case token_type::KEYWORD_RAISE: return "KEYWORD_RAISE";
        case token_type::KEYWORD_TRY: return "KEYWORD_TRY";
        case token_type::KEYWORD_WITH: return "KEYWORD_WITH";
        case token_type::KEYWORD_YIELD: return "KEYWORD_YIELD";
        case token_type::KEYWORD_MATCH: return "KEYWORD_MATCH";
        case token_type::KEYWORD_CASE: return "KEYWORD_CASE";
        case token_type::KEYWORD_UNDERSCORE: return "KEYWORD_UNDERSCORE";

        case token_type::OP_AND: return "OP_AND";
        case token_type::OP_OR: return "OP_OR";
        case token_type::OP_NOT: return "OP_NOT";
        case token_type::OP_IS: return "OP_IS";
        case token_type::OP_IN: return "OP_IN";

        case token_type::OP_ASSIGN: return "OP_ASSIGN";
        case token_type::OP_PLUS: return "OP_PLUS";
        case token_type::OP_MINUS: return "OP_MINUS";
        case token_type::OP_EQUAL: return "OP_EQUAL";
        case token_type::OP_NOT_EQUAL: return "OP_NOT_EQUAL";
        case token_type::OP_LESS: return "OP_LESS";
        case token_type::OP_GREATER: return "OP_GREATER";
        case token_type::OP_LESS_EQUAL: return "OP_LESS_EQUAL";
        case token_type::OP_GREATER_EQUAL: return "OP_GREATER_EQUAL";
        case token_type::OP_STAR: return "OP_STAR";
        case token_type::OP_SLASH: return "OP_SLASH";
        case token_type::OP_DOUBLE_SLASH: return "OP_DOUBLE_SLASH";
        case token_type::OP_PERCENT: return "OP_PERCENT";
        case token_type::OP_DOUBLE_STAR: return "OP_DOUBLE_STAR";
        case token_type::OP_AT: return "OP_AT";
        case token_type::OP_AMPERSAND: return "OP_AMPERSAND";
        case token_type::OP_PIPE: return "OP_PIPE";
        case token_type::OP_CARET: return "OP_CARET";
        case token_type::OP_TILDE: return "OP_TILDE";
        case token_type::OP_LEFT_SHIFT: return "OP_LEFT_SHIFT";
        case token_type::OP_RIGHT_SHIFT: return "OP_RIGHT_SHIFT";
        case token_type::OP_PLUS_ASSIGN: return "OP_PLUS_ASSIGN";
        case token_type::OP_MINUS_ASSIGN: return "OP_MINUS_ASSIGN";
        case token_type::OP_STAR_ASSIGN: return "OP_STAR_ASSIGN";
        case token_type::OP_SLASH_ASSIGN: return "OP_SLASH_ASSIGN";
        case token_type::OP_DOUBLE_SLASH_ASSIGN: return "OP_DOUBLE_SLASH_ASSIGN";
        case token_type::OP_PERCENT_ASSIGN: return "OP_PERCENT_ASSIGN";
        case token_type::OP_DOUBLE_STAR_ASSIGN: return "OP_DOUBLE_STAR_ASSIGN";
        case token_type::OP_AT_ASSIGN: return "OP_AT_ASSIGN";
        case token_type::OP_AMPERSAND_ASSIGN: return "OP_AMPERSAND_ASSIGN";
        case token_type::OP_PIPE_ASSIGN: return "OP_PIPE_ASSIGN";
        case token_type::OP_CARET_ASSIGN: return "OP_CARET_ASSIGN";
        case token_type::OP_RIGHT_SHIFT_ASSIGN: return "OP_RIGHT_SHIFT_ASSIGN";
        case token_type::OP_LEFT_SHIFT_ASSIGN: return "OP_LEFT_SHIFT_ASSIGN";
        case token_type::OP_WALRUS: return "OP_WALRUS";
        case token_type::OP_ARROW: return "OP_ARROW";

        case token_type::OPEN_PAREN: return "OPEN_PAREN";
        case token_type::CLOSE_PAREN: return "CLOSE_PAREN";
        case token_type::OPEN_BRACE: return "OPEN_BRACE";
        case token_type::CLOSE_BRACE: return "CLOSE_BRACE";
        case token_type::OPEN_BRACKET: return "OPEN_BRACKET";
        case token_type::CLOSE_BRACKET: return "CLOSE_BRACKET";

        case token_type::SEMICOLON: return "SEMICOLON";
        case token_type::COMMA: return "COMMA";
        case token_type::DOT: return "DOT";
        case token_type::COLON: return "COLON";
        case token_type::QUESTION: return "QUESTION";
        case token_type::COMMENT_SINGLE: return "COMMENT_SINGLE";
        case token_type::COMMENT_MULTI: return "COMMENT_MULTI";

        case token_type::TYPE_INT: return "TYPE_INT";
        case token_type::TYPE_FLOAT: return "TYPE_FLOAT";
        case token_type::TYPE_STRING: return "TYPE_STRING";
        case token_type::TYPE_BOOL: return "TYPE_BOOL";
        case token_type::TYPE_LIST: return "TYPE_LIST";
        case token_type::TYPE_DICT: return "TYPE_DICT";
        case token_type::TYPE_SET: return "TYPE_SET";
        case token_type::TYPE_TUPLE: return "TYPE_TUPLE";
        case token_type::TYPE_BYTES: return "TYPE_BYTES";
        case token_type::TYPE_COMPLEX: return "TYPE_COMPLEX";
        case token_type::TYPE_FROZENSET: return "TYPE_FROZENSET";
        case token_type::TYPE_BYTEARRAY: return "TYPE_BYTEARRAY";
        case token_type::TYPE_OBJECT: return "TYPE_OBJECT";
    }
    return "UNKNOWN";
}

} // namespace cythonpp::domain::lexer
