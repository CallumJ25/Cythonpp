#ifndef CYTHONPP_DOMAIN_LEXER_SCAN_CONTEXT_H
#define CYTHONPP_DOMAIN_LEXER_SCAN_CONTEXT_H

#include <cstddef>
#include <vector>

#include "token_type.h"

namespace cythonpp::domain::lexer {

// Tracks the two pieces of surrounding context the scanner cannot get from
// the character under the cursor: how deep it is inside brackets, and
// whether it is currently reading a type annotation.
//
// Split out of Lexer because it is pure state-transition logic with no
// cursor involvement, so it can be tested by feeding it token types
// directly instead of round-tripping through full source strings.
//
// All input is assumed to be valid Python; this makes no attempt to detect
// unbalanced brackets or misplaced colons.
class ScanContext {
public:
    // Fed every significant token as it is emitted, in order -- including
    // NEWLINE and SEMICOLON, which end a logical line and reset the state.
    // SPACE, TAB, COMMENT_SINGLE and TOKEN_EOF are not significant and must
    // not be passed: a comment must not make a blank line look like the
    // start of a statement.
    void observe(token_type type);

    // True when the scanner is reading a type annotation, so a builtin type
    // name should classify as TYPE_* rather than IDENTIFIER.
    bool in_annotation() const;

    // Non-zero inside (), [] or {}, where a newline is an implicit line
    // continuation rather than the end of a statement.
    std::size_t bracket_depth() const;

private:
    // One entry per currently-open bracket. `annotation` records the value
    // of annotation_ when the bracket opened, so `list[int]` keeps resolving
    // builtin names as TYPE_* while `f(int)` does not.
    struct BracketFrame {
        token_type opener;
        bool annotation;
    };

    void observe_colon();
    void observe_comma();
    void end_logical_line();

    bool annotation_ = false;
    // Set when the current logical line began with a compound-statement
    // keyword, and cleared by the colon that closes that header -- which is
    // how `if x:` is told apart from `x: int`.
    bool block_header_pending_ = false;
    bool line_has_content_ = false;
    std::vector<BracketFrame> brackets_;
    // Bracket depth of each `lambda` whose colon has not yet been seen, so
    // nested lambdas resolve their own colons.
    std::vector<std::size_t> lambda_depths_;
};

} // namespace cythonpp::domain::lexer

#endif // CYTHONPP_DOMAIN_LEXER_SCAN_CONTEXT_H
