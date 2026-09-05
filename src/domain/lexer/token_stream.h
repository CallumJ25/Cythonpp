#ifndef CYTHONPP_DOMAIN_LEXER_TOKEN_STREAM_H
#define CYTHONPP_DOMAIN_LEXER_TOKEN_STREAM_H

#include <cstddef>
#include <vector>

#include "token.h"

namespace cythonpp::domain::lexer {

// The tokens a Lexer produced for one source file, in source order: left to
// right, top to bottom.
//
// This exists so the stages downstream of the lexer name a domain concept
// instead of passing a bare std::vector<Token>. The vector's mutating half
// is not part of the contract a parser should have, and the things a parser
// will want next -- a read cursor, peek()/expect(), a skip-whitespace helper
// -- get a home here without changing any signature that already exists.
// It is deliberately minimal until there is a parser to need them.
class TokenStream {
public:
    TokenStream() = default;
    explicit TokenStream(std::vector<Token> tokens);

    using const_iterator = std::vector<Token>::const_iterator;

    const_iterator begin() const;
    const_iterator end() const;

    std::size_t size() const;
    bool empty() const;

    // Throws std::out_of_range if `index` is past the end, matching
    // std::vector::at, so a stray index fails loudly rather than silently.
    const Token& at(std::size_t index) const;

    const std::vector<Token>& tokens() const;

    // ---- read cursor ----
    //
    // The cursor's invariant is that it never rests on a COMMENT_SINGLE.
    // IndentationPass passes comment tokens through, and inside brackets the
    // lexer suppresses the newline, so a comment can sit in the middle of an
    // expression. advance(), seek() and construction all normalise forward
    // past them, which keeps peek() a plain index read instead of a scan.
    //
    // Because advance() mutates, a stream has one reader at a time. rewind()
    // makes it re-readable.

    // The token `lookahead` positions ahead of the cursor, skipping comments.
    // Clamps to the last token rather than running off the end -- lexer output
    // always ends in TOKEN_EOF, so clamping lands there. Throws
    // std::out_of_range on an empty stream, matching at().
    const Token& peek(std::size_t lookahead = 0) const;

    bool check(token_type type) const;

    // Advances only if the current token matches. Returns whether it did.
    bool match(token_type type);

    // Returns the current token and moves past it. A no-op at TOKEN_EOF, so a
    // parser loop cannot run away past the end.
    const Token& advance();

    bool at_end() const;

    std::size_t position() const;
    void seek(std::size_t position);
    void rewind();

private:
    // The first index at or after `index` that is not a comment.
    std::size_t skip_trivia(std::size_t index) const;

    std::vector<Token> tokens_;
    std::size_t position_ = 0;
};

} // namespace cythonpp::domain::lexer

#endif // CYTHONPP_DOMAIN_LEXER_TOKEN_STREAM_H
