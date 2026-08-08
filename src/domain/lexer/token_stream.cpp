#include "token_stream.h"

namespace cythonpp::domain::lexer {

TokenStream::TokenStream(std::vector<Token> tokens) : tokens_(std::move(tokens)) {}

TokenStream::const_iterator TokenStream::begin() const { return tokens_.begin(); }

TokenStream::const_iterator TokenStream::end() const { return tokens_.end(); }

std::size_t TokenStream::size() const { return tokens_.size(); }

bool TokenStream::empty() const { return tokens_.empty(); }

const Token& TokenStream::at(std::size_t index) const { return tokens_.at(index); }

const std::vector<Token>& TokenStream::tokens() const { return tokens_; }

} // namespace cythonpp::domain::lexer
