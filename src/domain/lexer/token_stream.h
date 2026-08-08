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

private:
    std::vector<Token> tokens_;
};

} // namespace cythonpp::domain::lexer

#endif // CYTHONPP_DOMAIN_LEXER_TOKEN_STREAM_H
