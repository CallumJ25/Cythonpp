#include "token_stream.h"

#include <algorithm>

namespace cythonpp::domain::lexer {

TokenStream::TokenStream(std::vector<Token> tokens) : tokens_(std::move(tokens)) {
    position_ = skip_trivia(0);
}

TokenStream::const_iterator TokenStream::begin() const { return tokens_.begin(); }

TokenStream::const_iterator TokenStream::end() const { return tokens_.end(); }

std::size_t TokenStream::size() const { return tokens_.size(); }

bool TokenStream::empty() const { return tokens_.empty(); }

const Token& TokenStream::at(std::size_t index) const { return tokens_.at(index); }

const std::vector<Token>& TokenStream::tokens() const { return tokens_; }

std::size_t TokenStream::skip_trivia(std::size_t index) const {
    while (index < tokens_.size() && tokens_[index].type() == token_type::COMMENT_SINGLE) {
        ++index;
    }
    return index;
}

const Token& TokenStream::peek(std::size_t lookahead) const {
    if (tokens_.empty()) {
        return tokens_.at(0); // always throws std::out_of_range
    }
    std::size_t index = position_;
    for (std::size_t step = 0; step < lookahead; ++step) {
        index = skip_trivia(index + 1);
    }
    if (index >= tokens_.size()) {
        index = tokens_.size() - 1;
    }
    return tokens_[index];
}

bool TokenStream::check(token_type type) const {
    return !tokens_.empty() && peek().type() == type;
}

bool TokenStream::match(token_type type) {
    if (!check(type)) {
        return false;
    }
    advance();
    return true;
}

const Token& TokenStream::advance() {
    const Token& current = peek();
    if (!at_end()) {
        position_ = skip_trivia(position_ + 1);
    }
    return current;
}

bool TokenStream::at_end() const {
    // The position test comes first so an all-comment stream -- which the
    // lexer cannot produce, but a hand-built one can -- is still at its end.
    return position_ >= tokens_.size() || tokens_[position_].type() == token_type::TOKEN_EOF;
}

std::size_t TokenStream::position() const { return position_; }

void TokenStream::seek(std::size_t position) {
    position_ = skip_trivia(std::min(position, tokens_.size()));
}

void TokenStream::rewind() { seek(0); }

} // namespace cythonpp::domain::lexer
