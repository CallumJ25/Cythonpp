#include "scan_context.h"

namespace cythonpp::domain::lexer {

namespace {

// Reserved words that open a compound statement, i.e. whose logical line
// ends in a block-introducing colon rather than an annotation colon.
bool is_block_keyword(token_type type) {
    switch (type) {
        case token_type::KEYWORD_IF:
        case token_type::KEYWORD_ELIF:
        case token_type::KEYWORD_ELSE:
        case token_type::KEYWORD_FOR:
        case token_type::KEYWORD_WHILE:
        case token_type::KEYWORD_WITH:
        case token_type::KEYWORD_TRY:
        case token_type::KEYWORD_EXCEPT:
        case token_type::KEYWORD_FINALLY:
        case token_type::KEYWORD_DEF:
        case token_type::KEYWORD_CLASS:
        case token_type::KEYWORD_ASYNC:
            return true;
        default:
            return false;
    }
}

bool is_open_bracket(token_type type) {
    return type == token_type::OPEN_PAREN || type == token_type::OPEN_BRACKET ||
           type == token_type::OPEN_BRACE;
}

bool is_close_bracket(token_type type) {
    return type == token_type::CLOSE_PAREN || type == token_type::CLOSE_BRACKET ||
           type == token_type::CLOSE_BRACE;
}

} // namespace

void ScanContext::observe(token_type type) {
    if (!line_has_content_) {
        block_header_pending_ = is_block_keyword(type);
        line_has_content_ = true;
    }

    if (is_open_bracket(type)) {
        // annotation_ itself is unchanged: list[int] must keep resolving
        // its contents as type names.
        brackets_.push_back({type, annotation_});
        return;
    }
    if (is_close_bracket(type)) {
        if (!brackets_.empty()) {
            annotation_ = brackets_.back().annotation;
            brackets_.pop_back();
        }
        return;
    }

    switch (type) {
        case token_type::COLON:
            observe_colon();
            return;
        case token_type::OP_ARROW:
            annotation_ = true;
            return;
        case token_type::OP_ASSIGN:
            annotation_ = false;
            return;
        case token_type::COMMA:
            observe_comma();
            return;
        case token_type::KEYWORD_LAMBDA:
            lambda_depths_.push_back(brackets_.size());
            return;
        case token_type::NEWLINE:
        case token_type::SEMICOLON:
            end_logical_line();
            return;
        default:
            return;
    }
}

void ScanContext::observe_colon() {
    // A lambda's colon closes its parameter list; it never introduces an
    // annotation, and it is matched at the depth the lambda opened at so
    // that nesting works.
    if (!lambda_depths_.empty() && lambda_depths_.back() == brackets_.size()) {
        lambda_depths_.pop_back();
        return;
    }

    if (!brackets_.empty()) {
        const token_type opener = brackets_.back().opener;
        if (opener == token_type::OPEN_BRACKET || opener == token_type::OPEN_BRACE) {
            // A slice colon (a[1:2]) or a dict-literal colon ({"k": 1}).
            return;
        }
        // Inside parentheses the only legal bare colon is a parameter
        // annotation -- a lambda's was handled above, and anything else is
        // a SyntaxError.
        annotation_ = true;
        return;
    }

    if (block_header_pending_) {
        // Consumed rather than merely read, so the second colon on a line
        // like `if cond: x: int = 5` is correctly an annotation colon.
        block_header_pending_ = false;
        annotation_ = false;
        return;
    }

    annotation_ = true;
}

void ScanContext::observe_comma() {
    // In `def f(a: int, b)` the comma sits inside a '(' opened outside
    // annotation context, so it ends the annotation. In `dict[str, int]` it
    // sits inside a '[' opened *inside* annotation context, so the
    // annotation survives and `int` still resolves as a type name.
    if (brackets_.empty() || !brackets_.back().annotation) {
        annotation_ = false;
    }
}

void ScanContext::end_logical_line() {
    annotation_ = false;
    block_header_pending_ = false;
    line_has_content_ = false;
    lambda_depths_.clear();
    // brackets_ is necessarily empty here: the scanner only emits a NEWLINE
    // at depth zero.
}

bool ScanContext::in_annotation() const { return annotation_; }

std::size_t ScanContext::bracket_depth() const { return brackets_.size(); }

} // namespace cythonpp::domain::lexer
