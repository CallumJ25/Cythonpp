#include "lexer.h"

#include <cctype>
#include <string_view>

#include "keyword_table.h"
#include "operator_table.h"

namespace cythonpp::domain::lexer {

namespace {

bool is_digit(char c) { return c >= '0' && c <= '9'; }

bool is_hex_digit(char c) {
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

// Bytes >= 0x80 are accepted so a non-ASCII identifier becomes one
// IDENTIFIER token rather than a run of TOKEN_ERRORs. No UTF-8 validation
// is attempted.
bool is_identifier_start(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' ||
           static_cast<unsigned char>(c) >= 0x80;
}

bool is_identifier_continue(char c) { return is_identifier_start(c) || is_digit(c); }

char to_lower(char c) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

// The legal string prefixes, case-insensitive. `u` combines with nothing,
// and `bf`/`uf` are not legal, so keeping the accept-set literal handles
// those exclusions for free.
bool is_string_prefix(std::string_view text) {
    if (text.empty() || text.size() > 2) {
        return false;
    }
    char first = to_lower(text[0]);
    if (text.size() == 1) {
        return first == 'r' || first == 'b' || first == 'u' || first == 'f';
    }
    char second = to_lower(text[1]);
    return (first == 'r' && second == 'b') || (first == 'b' && second == 'r') ||
           (first == 'f' && second == 'r') || (first == 'r' && second == 'f');
}

bool prefix_contains(std::string_view prefix, char wanted) {
    for (char c : prefix) {
        if (to_lower(c) == wanted) {
            return true;
        }
    }
    return false;
}

} // namespace

Lexer::Lexer(std::string source) : source_(std::move(source)) {}

void Lexer::reset() {
    tokens_.clear();
    context_ = ScanContext();
    position_ = 0;
    line_ = 1;
    column_ = 1;
    at_line_start_ = true;
    line_has_content_ = false;
}

bool Lexer::at_end() const { return position_ >= source_.size(); }

// Reading past the end yields '\0', which is never a legal Python source
// character, so the scanners need no bounds checks of their own.
char Lexer::peek(std::size_t offset) const {
    const std::size_t index = position_ + offset;
    return index < source_.size() ? source_[index] : '\0';
}

// The single choke point for position bookkeeping: because every consuming
// path goes through here, line and column stay correct across newlines
// inside triple-quoted strings and line continuations without any scanner
// having to remember to handle them.
char Lexer::advance() {
    const char c = source_[position_++];
    if (c == '\n') {
        ++line_;
        column_ = 1;
    } else if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) {
        // Continuation bytes of a UTF-8 sequence carry no column of their
        // own, so a multi-byte character advances the column once rather
        // than once per byte. Checking for them is safe without decoding:
        // UTF-8 is self-synchronizing, so 10xxxxxx only ever appears inside
        // a multi-byte character and never as an ASCII byte.
        ++column_;
    }
    return c;
}

void Lexer::emit(token_type type, std::string lexeme, int line, int column) {
    const bool significant = type != token_type::SPACE && type != token_type::TAB &&
                             type != token_type::COMMENT_SINGLE && type != token_type::TOKEN_EOF;
    if (significant) {
        line_has_content_ = true;
        context_.observe(type);
    }
    tokens_.emplace_back(type, std::move(lexeme), line, column, line_, column_);
}

void Lexer::emit_from(token_type type, std::size_t start, int line, int column) {
    emit(type, source_.substr(start, position_ - start), line, column);
}

std::vector<Token> Lexer::tokenize() {
    reset();

    while (!at_end()) {
        if (at_line_start_) {
            scan_line_start();
            continue;
        }

        const char c = peek();
        if (c == ' ' || c == '\t' || c == '\r' || c == '\f') {
            advance();
            continue;
        }
        if (c == '\n') {
            scan_end_of_line();
            continue;
        }
        if (c == '\\' && (peek(1) == '\n' || (peek(1) == '\r' && peek(2) == '\n'))) {
            scan_line_continuation();
            continue;
        }
        scan_token();
    }

    finish();
    return std::move(tokens_);
}

void Lexer::scan_token() {
    const char c = peek();
    if (c == '#') {
        scan_comment();
        return;
    }
    if (c == '\'' || c == '"') {
        scan_string(position_, line_, column_);
        return;
    }
    if (is_identifier_start(c)) {
        scan_word();
        return;
    }
    if (is_digit(c) || (c == '.' && is_digit(peek(1)))) {
        scan_number();
        return;
    }
    scan_operator();
}

void Lexer::scan_line_start() {
    // Probe the whitespace run before emitting anything: CPython ignores the
    // indentation of blank and comment-only lines entirely, so tokenizing it
    // here would put non-indentation whitespace into the stream and mislead
    // the future INDENT/DEDENT pass.
    std::size_t probe = position_;
    while (probe < source_.size() &&
           (source_[probe] == ' ' || source_[probe] == '\t' || source_[probe] == '\f')) {
        ++probe;
    }
    const bool blank = probe >= source_.size() || source_[probe] == '\n' ||
                       source_[probe] == '\r' || source_[probe] == '#';

    if (!blank) {
        while (peek() == ' ' || peek() == '\t') {
            const int line = line_;
            const int column = column_;
            const char c = advance();
            emit(c == ' ' ? token_type::SPACE : token_type::TAB, std::string(1, c), line, column);
        }
    }
    while (peek() == ' ' || peek() == '\t' || peek() == '\f') {
        advance();
    }

    at_line_start_ = false;
}

void Lexer::scan_end_of_line() {
    const int line = line_;
    const int column = column_;
    advance();

    if (context_.bracket_depth() > 0) {
        // Implicit continuation inside ( [ {: no NEWLINE, and the next
        // line's leading whitespace is not indentation.
        at_line_start_ = false;
        return;
    }

    // A blank or comment-only line produces no token at all. Emitting one
    // would break the invariant a parser wants -- every NEWLINE terminates a
    // statement -- and the information is not lost, since Token carries
    // line_number and a consumer can compare across the gap.
    if (line_has_content_) {
        emit(token_type::NEWLINE, "\n", line, column);
        line_has_content_ = false;
    }
    at_line_start_ = true;
}

void Lexer::scan_line_continuation() {
    advance();
    if (peek() == '\r') {
        advance();
    }
    advance();
    at_line_start_ = false;
}

void Lexer::scan_comment() {
    const std::size_t start = position_;
    const int line = line_;
    const int column = column_;
    while (!at_end() && peek() != '\n' && peek() != '\r') {
        advance();
    }
    emit_from(token_type::COMMENT_SINGLE, start, line, column);
}

void Lexer::scan_word() {
    const std::size_t start = position_;
    const int line = line_;
    const int column = column_;
    while (is_identifier_continue(peek())) {
        advance();
    }
    const std::string_view text(source_.data() + start, position_ - start);

    // A string prefix must be recognised before keyword lookup, because
    // f"..." and rb'...' begin with an identifier-shaped run.
    if ((peek() == '"' || peek() == '\'') && is_string_prefix(text)) {
        if (prefix_contains(text, 'f')) {
            scan_fstring(start, line, column);
        } else {
            scan_string(start, line, column);
        }
        return;
    }

    emit(classify_word(text, context_.in_annotation()), std::string(text), line, column);
}

void Lexer::scan_number() {
    const std::size_t start = position_;
    const int line = line_;
    const int column = column_;
    token_type type = token_type::LITERAL_INT;

    const char base = to_lower(peek(1));
    if (peek() == '0' && (base == 'x' || base == 'o' || base == 'b')) {
        advance();
        advance();
        while (is_hex_digit(peek()) || peek() == '_') {
            advance();
        }
    } else {
        while (is_digit(peek()) || peek() == '_') {
            advance();
        }
        if (peek() == '.') {
            type = token_type::LITERAL_FLOAT;
            advance();
            while (is_digit(peek()) || peek() == '_') {
                advance();
            }
        }
        // The exponent only starts if a digit or a signed digit follows,
        // which is what stops `1if x else 2` and a trailing `1e` from
        // swallowing the wrong characters.
        if (to_lower(peek()) == 'e' &&
            (is_digit(peek(1)) || ((peek(1) == '+' || peek(1) == '-') && is_digit(peek(2))))) {
            type = token_type::LITERAL_FLOAT;
            advance();
            advance();
            while (is_digit(peek()) || peek() == '_') {
                advance();
            }
        }
    }

    if (to_lower(peek()) == 'j') {
        advance();
        type = token_type::LITERAL_COMPLEX;
    }

    // The lexeme is the raw source slice, underscores and base prefix
    // included. No numeric value is computed: whether 0xFFFFFFFFFFFFFFFFFF
    // fits in an int is a semantic question, and answering it here would
    // need the error handling this stage deliberately does not do.
    emit_from(type, start, line, column);
}

void Lexer::scan_string(std::size_t start, int line, int column) {
    const std::string_view prefix(source_.data() + start, position_ - start);
    const bool bytes = prefix_contains(prefix, 'b');

    const char quote = peek();
    const bool triple = peek(1) == quote && peek(2) == quote;
    advance();
    if (triple) {
        advance();
        advance();
    }

    while (!at_end()) {
        const char c = peek();
        if (c == '\\') {
            // A backslash always hides the next character from the
            // terminator search, raw strings included: r"\"" is a complete
            // string even though the backslash is kept in its value. Raw-ness
            // only affects decoding, and nothing here decodes.
            advance();
            if (!at_end()) {
                advance();
            }
            continue;
        }
        if (!triple && c == '\n') {
            // Unterminated. Stop at the newline so a stray quote costs one
            // line instead of swallowing the rest of the file.
            break;
        }
        if (c == quote) {
            if (!triple) {
                advance();
                break;
            }
            if (peek(1) == quote && peek(2) == quote) {
                advance();
                advance();
                advance();
                break;
            }
        }
        advance();
    }

    emit_from(bytes ? token_type::LITERAL_BYTES : token_type::LITERAL_STRING, start, line, column);
}

void Lexer::scan_fstring(std::size_t start, int line, int column) {
    const char quote = peek();
    const bool triple = peek(1) == quote && peek(2) == quote;
    advance();
    if (triple) {
        advance();
        advance();
    }
    emit_from(token_type::FSTRING_START, start, line, column);

    std::size_t chunk_start = position_;
    int chunk_line = line_;
    int chunk_column = column_;

    while (!at_end()) {
        const char c = peek();
        if (c == '\\') {
            advance();
            if (!at_end()) {
                advance();
            }
            continue;
        }
        if (!triple && c == '\n') {
            break;
        }
        // Doubled braces are literal text, not a replacement field.
        if ((c == '{' && peek(1) == '{') || (c == '}' && peek(1) == '}')) {
            advance();
            advance();
            continue;
        }
        if (c == quote && (!triple || (peek(1) == quote && peek(2) == quote))) {
            break;
        }
        if (c == '{') {
            if (position_ > chunk_start) {
                emit_from(token_type::FSTRING_MIDDLE, chunk_start, chunk_line, chunk_column);
            }
            scan_fstring_replacement_field(quote, triple);
            chunk_start = position_;
            chunk_line = line_;
            chunk_column = column_;
            continue;
        }
        advance();
    }

    if (position_ > chunk_start) {
        emit_from(token_type::FSTRING_MIDDLE, chunk_start, chunk_line, chunk_column);
    }

    const std::size_t end_start = position_;
    const int end_line = line_;
    const int end_column = column_;
    if (peek() == quote) {
        advance();
        if (triple) {
            if (peek() == quote) {
                advance();
            }
            if (peek() == quote) {
                advance();
            }
        }
    }
    emit_from(token_type::FSTRING_END, end_start, end_line, end_column);
}

void Lexer::scan_fstring_replacement_field(char quote, bool triple) {
    const int line = line_;
    const int column = column_;
    advance();
    emit(token_type::OPEN_BRACE, "{", line, column);

    // The field's own depth, so a nested dict or call inside it does not
    // look like the field's closing brace.
    const std::size_t field_depth = context_.bracket_depth();

    while (!at_end()) {
        const char c = peek();
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            advance();
            continue;
        }

        if (context_.bracket_depth() == field_depth) {
            if (c == '}') {
                break;
            }
            if (c == ':') {
                const int colon_line = line_;
                const int colon_column = column_;
                advance();
                emit(token_type::COLON, ":", colon_line, colon_column);
                scan_fstring_format_spec(quote, triple);
                break;
            }
            // The !r / !s / !a conversion, and the f"{x=}" debug specifier:
            // formatting syntax rather than expression, so each is one
            // FSTRING_MIDDLE. Guarding on the next character keeps `!=` and
            // `==` on the ordinary operator path.
            if ((c == '!' || c == '=') && peek(1) != '=') {
                const std::size_t start = position_;
                const int part_line = line_;
                const int part_column = column_;
                advance();
                if (c == '!' && !at_end()) {
                    advance();
                }
                emit_from(token_type::FSTRING_MIDDLE, start, part_line, part_column);
                continue;
            }
        }

        scan_token();
    }

    if (peek() == '}') {
        const int close_line = line_;
        const int close_column = column_;
        advance();
        emit(token_type::CLOSE_BRACE, "}", close_line, close_column);
    }
}

void Lexer::scan_fstring_format_spec(char quote, bool triple) {
    // A format spec is literal text, except for nested replacement fields
    // such as the width in f"{value:>{width}}".
    std::size_t chunk_start = position_;
    int chunk_line = line_;
    int chunk_column = column_;

    while (!at_end()) {
        const char c = peek();
        if (c == '}') {
            break;
        }
        if (!triple && c == '\n') {
            break;
        }
        if (c == quote && (!triple || (peek(1) == quote && peek(2) == quote))) {
            break;
        }
        if (c == '{') {
            if (position_ > chunk_start) {
                emit_from(token_type::FSTRING_MIDDLE, chunk_start, chunk_line, chunk_column);
            }
            scan_fstring_replacement_field(quote, triple);
            chunk_start = position_;
            chunk_line = line_;
            chunk_column = column_;
            continue;
        }
        advance();
    }

    if (position_ > chunk_start) {
        emit_from(token_type::FSTRING_MIDDLE, chunk_start, chunk_line, chunk_column);
    }
}

void Lexer::scan_operator() {
    const int line = line_;
    const int column = column_;
    const std::string_view remaining(source_.data() + position_, source_.size() - position_);
    const OperatorMatch match = longest_operator_at(remaining);

    if (match.length == 0) {
        // No throw: a complete stream with one bad token is more useful than
        // an unwind that discards everything scanned so far, and it keeps
        // tokenize() total so callers need no try/catch.
        const std::size_t start = position_;
        const int bad_line = line_;
        const int bad_column = column_;
        advance();
        emit_from(token_type::TOKEN_ERROR, start, bad_line, bad_column);
        return;
    }

    const std::size_t start = position_;
    for (std::size_t index = 0; index < match.length; ++index) {
        advance();
    }
    emit_from(match.type, start, line, column);
}

void Lexer::finish() {
    // CPython closes an unterminated final line with a NEWLINE, so the
    // parser sees the same shape whether or not the file ends in one. The
    // empty lexeme marks it as synthesized rather than scanned.
    if (line_has_content_) {
        emit(token_type::NEWLINE, "", line_, column_);
        line_has_content_ = false;
    }
    emit(token_type::TOKEN_EOF, "", line_, column_);
}

} // namespace cythonpp::domain::lexer
