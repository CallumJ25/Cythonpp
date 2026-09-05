#ifndef CYTHONPP_DOMAIN_AST_SOURCE_SPAN_H
#define CYTHONPP_DOMAIN_AST_SOURCE_SPAN_H

#include "domain/lexer/token.h"

namespace cythonpp::domain::ast {

// The region of source an AST node came from.
//
// The end is half-open -- one past the last character -- because that is what
// the lexer's cursor already holds when a token is emitted. Nothing here
// recomputes positions, so there is no second place that has to agree with
// Lexer::advance() about how UTF-8 characters are counted.
struct SourceSpan {
    int start_line;
    int start_column;
    int end_line;
    int end_column;
};

inline bool operator==(SourceSpan left, SourceSpan right) {
    return left.start_line == right.start_line && left.start_column == right.start_column &&
           left.end_line == right.end_line && left.end_column == right.end_column;
}

inline bool operator!=(SourceSpan left, SourceSpan right) { return !(left == right); }

inline SourceSpan span_of(const lexer::Token& token) {
    return SourceSpan{token.line_number(), token.column_number(), token.end_line(),
                      token.end_column()};
}

// The start of `first` and the end of `last`, for a node built from a run of
// tokens. Ordering is not validated: the parser is the only caller and always
// has them in source order, so a check here would be a test for the parser
// living in the wrong file.
inline SourceSpan merge(SourceSpan first, SourceSpan last) {
    return SourceSpan{first.start_line, first.start_column, last.end_line, last.end_column};
}

} // namespace cythonpp::domain::ast

#endif // CYTHONPP_DOMAIN_AST_SOURCE_SPAN_H
