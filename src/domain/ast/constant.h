#ifndef CYTHONPP_DOMAIN_AST_CONSTANT_H
#define CYTHONPP_DOMAIN_AST_CONSTANT_H

#include <string>
#include <utility>

#include "domain/lexer/token_type.h"
#include "expr.h"
#include "visitor.h"

namespace cythonpp::domain::ast {

// A literal, stored exactly as it was written.
//
// No decoding happens here. Python integers are arbitrary precision, so
// deciding a literal fits in int64_t is a typing decision rather than a
// syntactic one, and string escape handling depends on the r/b/f prefix,
// which is semantic. Keeping the raw text also means diagnostics can quote
// what the user actually wrote.
class Constant : public Expr {
public:
    Constant(SourceSpan span, lexer::token_type type, std::string lexeme)
        : Expr(span), type_(type), lexeme_(std::move(lexeme)) {}

    lexer::token_type type() const { return type_; }
    const std::string& lexeme() const { return lexeme_; }

    void accept(Visitor& visitor) const override { visitor.visit(*this); }

private:
    lexer::token_type type_;
    std::string lexeme_;
};

} // namespace cythonpp::domain::ast

#endif // CYTHONPP_DOMAIN_AST_CONSTANT_H
