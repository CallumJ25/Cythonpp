#include "domain/lexer/indentation_pass.h"

#include <utility>
#include <vector>

#include "token.h"
#include "token_type.h"

namespace cythonpp::domain::lexer {

namespace {

// CPython's col/altcol pair. `col` expands a tab to the next tab stop;
// `alt_col` counts a tab as a single column. Indentation whose ordering
// depends on which of the two you believe is precisely what TabError means,
// which is why both are carried rather than picking one.
struct IndentLevel {
    int col = 0;
    int alt_col = 0;
};

constexpr int TAB_STOP = 8;

class Builder {
public:
    explicit Builder(diagnostics::DiagnosticSink& sink) : sink_(sink) {
        // The sentinel. Never popped and never overwritten: every DEDENT has
        // to answer to an INDENT, and the base level never had one.
        levels_.push_back(IndentLevel{});
    }

    std::vector<Token> run(const std::vector<Token>& tokens);

private:
    void close_line(const Token& next);
    void report_tab_error(const Token& at);
    void emit(token_type type, const Token& at);
    void flush(const Token& at);

    diagnostics::DiagnosticSink& sink_;
    std::vector<IndentLevel> levels_;
    std::vector<Token> out_;
    IndentLevel pending_;
    // True at the start of the file as well as after every NEWLINE: the first
    // logical line has no NEWLINE in front of it but still has indentation.
    bool measuring_ = true;
};

std::vector<Token> Builder::run(const std::vector<Token>& tokens) {
    out_.reserve(tokens.size());

    for (const Token& token : tokens) {
        switch (token.type()) {
            case token_type::SPACE:
                pending_.col += 1;
                pending_.alt_col += 1;
                continue; // whitespace never reaches the output
            case token_type::TAB:
                // Advance to the next tab stop, not add eight: "  \t" is
                // column 8, not 10.
                pending_.col = (pending_.col / TAB_STOP + 1) * TAB_STOP;
                pending_.alt_col += 1;
                continue;
            case token_type::COMMENT_SINGLE:
                // Trivia. A comment-only line emits no NEWLINE and no
                // whitespace, so it is invisible as a line boundary and must
                // neither open nor close one.
                out_.push_back(token);
                continue;
            case token_type::NEWLINE:
                out_.push_back(token);
                pending_ = IndentLevel{};
                measuring_ = true;
                continue;
            case token_type::TOKEN_EOF:
                flush(token);
                out_.push_back(token);
                continue;
            default:
                break;
        }

        // The first significant token of a logical line, and so the point at
        // which the measurement accumulated above gets spent. Note the run and
        // this token can be on different physical lines -- a backslash
        // continuation puts them one apart -- so lines are grouped by token
        // adjacency and never by line_number().
        if (measuring_) {
            close_line(token);
        }
        out_.push_back(token);
    }

    // A well-formed stream ends in TOKEN_EOF and was flushed above. Flush
    // again for one that does not, because the balance guarantee is
    // unconditional. levels_ being deeper than the sentinel implies an INDENT
    // was emitted, which implies out_ is non-empty.
    if (levels_.size() > 1) {
        flush(out_.back());
    }

    return std::move(out_);
}

void Builder::close_line(const Token& next) {
    measuring_ = false;
    const IndentLevel line = pending_;

    if (line.col == levels_.back().col) {
        if (line.alt_col != levels_.back().alt_col) {
            report_tab_error(next);
        }
        return;
    }

    if (line.col > levels_.back().col) {
        if (line.alt_col <= levels_.back().alt_col) {
            report_tab_error(next);
        }
        // Pushed even after a TabError: recovery continues under the `col`
        // interpretation, and col is strictly increasing here, so the stack
        // stays ordered and the stream stays balanced.
        levels_.push_back(line);
        emit(token_type::INDENT, next);
        return;
    }

    // Guarded on size() > 1 so the sentinel is never popped. An unguarded pop
    // here is undefined behaviour, not merely a bad error message.
    while (levels_.size() > 1 && line.col < levels_.back().col) {
        levels_.pop_back();
        emit(token_type::DEDENT, next);
    }

    if (line.col != levels_.back().col) {
        sink_.report_error("IndentationError", "unindent does not match any outer indentation level",
                           next.line_number(), next.column_number());
        // Accept this line as the current level so one bad line does not
        // cascade into every line below it. Only ever above the sentinel:
        // overwriting the base would make it poppable, and the next dedent
        // would then emit a DEDENT with no INDENT to answer to.
        if (levels_.size() > 1) {
            levels_.back() = line;
        }
        return;
    }

    if (line.alt_col != levels_.back().alt_col) {
        report_tab_error(next);
    }
}

void Builder::report_tab_error(const Token& at) {
    sink_.report_error("TabError", "inconsistent use of tabs and spaces in indentation",
                       at.line_number(), at.column_number());
}

void Builder::emit(token_type type, const Token& at) {
    // Empty lexeme marks a synthesized token, the same convention the scanner
    // uses for the NEWLINE it invents at end of file.
    out_.emplace_back(type, "", at.line_number(), at.column_number());
}

void Builder::flush(const Token& at) {
    // size() - 1 dedents, not size(): the sentinel never had a matching INDENT.
    while (levels_.size() > 1) {
        levels_.pop_back();
        emit(token_type::DEDENT, at);
    }
}

} // namespace

TokenStream IndentationPass::run(const TokenStream& tokens, diagnostics::DiagnosticSink& sink) const {
    return TokenStream(Builder(sink).run(tokens.tokens()));
}

} // namespace cythonpp::domain::lexer
