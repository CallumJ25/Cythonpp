#ifndef CYTHONPP_DOMAIN_LEXER_LEXER_H
#define CYTHONPP_DOMAIN_LEXER_LEXER_H

#include <cstddef>
#include <string>
#include <vector>

#include "scan_context.h"
#include "token.h"
#include "token_type.h"

namespace cythonpp::domain::lexer {

// Turns Python source text into a flat token sequence.
//
// The scanner does no grammar validation: an unrecognised character becomes
// a TOKEN_ERROR and scanning continues, so a caller always receives a
// complete stream and one stray character costs one bad token rather than
// desynchronising the rest of the file.
//
// Whitespace policy: leading whitespace on a logical line is emitted as one
// SPACE or TAB token per character, preserving exactly which character was
// used so that a later pass can apply Python's tab/space rules and report
// TabError. Whitespace elsewhere -- between tokens, on a blank or
// comment-only line, or at the start of a continuation line -- is not
// tokenized, because it is not indentation.
//
// Positions are 1-based lines and 1-based columns, and a token records the
// position of its first character plus a half-open end position -- one past
// its last character. Columns count UTF-8 characters rather
// than bytes, matching what CPython's tokenize reports and where an editor
// caret lands, so a non-ASCII identifier earlier on the line does not skew
// every column after it. Note this counts code points, not grapheme
// clusters: a combining accent still counts as its own column.
//
// Lexemes are raw source bytes, so non-ASCII content in identifiers and
// strings round-trips exactly.
//
// INDENT/DEDENT tokens and TabError detection are not produced here. They are
// IndentationPass's job, which consumes the SPACE/TAB tokens above; see
// indentation_pass.h. Blank and comment-only lines are already excluded from
// the whitespace this emits, as CPython does, so the pass never sees them.
class Lexer {
public:
    explicit Lexer(std::string source);

    // Safe to call more than once; scanning state is reset on entry.
    std::vector<Token> tokenize();

private:
    void reset();

    bool at_end() const;
    char peek(std::size_t offset = 0) const;
    char advance();

    // Every token is created here so the "significant token" bookkeeping and
    // the ScanContext feed live in exactly one place.
    //
    // Precondition: the cursor (line_/column_) must sit exactly one past the
    // token's last character when this is called, since the end position is
    // read from line_/column_ at call time rather than recomputed from the
    // lexeme. Every call site advances past the token's text before calling
    // emit()/emit_from().
    void emit(token_type type, std::string lexeme, int line, int column);
    void emit_from(token_type type, std::size_t start, int line, int column);

    void scan_line_start();
    void scan_end_of_line();
    void scan_line_continuation();
    void scan_comment();
    void scan_word();
    void scan_number();
    void scan_string(std::size_t start, int line, int column);
    void scan_fstring(std::size_t start, int line, int column);
    void scan_fstring_replacement_field(char quote, bool triple);
    void scan_fstring_format_spec(char quote, bool triple);
    void scan_operator();
    void finish();

    // Consumes one token at the cursor, whatever it is. Shared by the main
    // loop and by f-string replacement fields, which lex ordinary Python.
    void scan_token();

    std::string source_;
    std::vector<Token> tokens_;
    ScanContext context_;
    std::size_t position_ = 0;
    int line_ = 1;
    int column_ = 1;
    bool at_line_start_ = true;
    bool line_has_content_ = false;
};

} // namespace cythonpp::domain::lexer

#endif // CYTHONPP_DOMAIN_LEXER_LEXER_H
