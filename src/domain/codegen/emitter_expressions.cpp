#include "domain/codegen/emitter.h"

#include <string>

#include "domain/ast/ann_assign.h"
#include "domain/ast/attribute.h"
#include "domain/ast/bin_op.h"
#include "domain/ast/bool_op.h"
#include "domain/ast/call.h"
#include "domain/ast/compare.h"
#include "domain/ast/constant.h"
#include "domain/ast/dict_expr.h"
#include "domain/ast/list_comp.h"
#include "domain/ast/list_expr.h"
#include "domain/ast/name.h"
#include "domain/ast/subscript.h"
#include "domain/ast/tuple_expr.h"
#include "domain/ast/unary_op.h"
#include "domain/codegen/cpp_type_name.h"
#include "domain/codegen/emit_diagnostic_kind.h"
#include "domain/codegen/name_mangler.h"
#include "domain/lexer/token_type.h"

namespace cythonpp::domain::codegen {
namespace {

// Python allows underscores as digit separators; C++ uses a different
// character, so they are stripped rather than passed through.
std::string without_underscores(const std::string& lexeme) {
    std::string digits;
    for (const char c : lexeme) {
        if (c != '_') {
            digits += c;
        }
    }
    return digits;
}

bool is_decimal_digits(const std::string& text) {
    if (text.empty()) {
        return false;
    }
    for (const char c : text) {
        if (c < '0' || c > '9') {
            return false;
        }
    }
    return true;
}

// Decodes a Python string literal's lexeme -- quotes and escapes included --
// into the bytes it denotes. Returns nullopt for any prefix or escape this
// slice does not model, so an unsupported escape is a refusal rather than a
// wrong string.
std::optional<std::string> decode_string_literal(const std::string& lexeme) {
    std::size_t index = 0;
    // A prefix (r, b, f, u and their combinations) changes what the literal
    // MEANS, so none is accepted here rather than being ignored.
    if (index >= lexeme.size() || (lexeme[index] != '"' && lexeme[index] != '\'')) {
        return std::nullopt;
    }
    const char quote = lexeme[index];
    // Triple-quoted literals are not modelled by this slice.
    if (lexeme.size() >= 6 && lexeme[1] == quote && lexeme[2] == quote) {
        return std::nullopt;
    }
    if (lexeme.size() < 2 || lexeme.back() != quote) {
        return std::nullopt;
    }
    std::string bytes;
    for (index = 1; index + 1 < lexeme.size(); ++index) {
        const char c = lexeme[index];
        if (c != '\\') {
            bytes += c;
            continue;
        }
        // The escape's target (index + 1) must leave the lexeme's own last
        // character -- the terminating quote -- unconsumed. If the target
        // WOULD BE that last character, the "closing quote" the earlier
        // `lexeme.back() == quote` check saw was actually eaten by this very
        // escape, so there is no real terminator at all: Lexer::scan_string
        // still emits a complete LITERAL_STRING token in that case (it only
        // stops at an unescaped newline), so an unterminated literal like
        // source `"a\"` + newline reaches here as the 4-byte lexeme `"a\"`
        // rather than as an error token. Refuse rather than let the
        // backslash silently swallow the terminator and decode a shorter,
        // wrong string. Written as an addition (`index + 2 >= lexeme.size()`)
        // rather than `index >= lexeme.size() - 2` so it cannot underflow for
        // a short lexeme; the loop guard above already limits index so this
        // can only ever be reached with lexeme.size() >= 3.
        if (index + 2 >= lexeme.size()) {
            return std::nullopt;
        }
        const char escape = lexeme[++index];
        switch (escape) {
        case 'n': bytes += '\n'; break;
        case 't': bytes += '\t'; break;
        case 'r': bytes += '\r'; break;
        case '0': bytes += '\0'; break;
        case '\\': bytes += '\\'; break;
        case '\'': bytes += '\''; break;
        case '"': bytes += '"'; break;
        default:
            // \x, \u, \U, \N{...} and line continuations are deliberately not
            // decoded here. Refusing is the honest answer; guessing produces
            // a wrong string that nothing downstream can detect.
            return std::nullopt;
        }
    }
    return bytes;
}

// Re-encodes bytes as a C++ string literal body using three-digit OCTAL
// escapes.
//
// OCTAL AND NOT HEX, deliberately: a C++ \x escape is GREEDY and consumes
// every following hex digit, so the two bytes 0x41 'B' would re-read as the
// single escape \x41B. \NNN is exactly three digits and cannot run on. The
// emitted literal is unreadable either way, and correctness wins.
std::string as_octal_escapes(const std::string& bytes) {
    std::string text;
    for (const char byte : bytes) {
        const unsigned value = static_cast<unsigned char>(byte);
        text += '\\';
        text += static_cast<char>('0' + ((value >> 6) & 0x7));
        text += static_cast<char>('0' + ((value >> 3) & 0x7));
        text += static_cast<char>('0' + (value & 0x7));
    }
    return text;
}

} // namespace

Emitter::Emitter(const semantic::TypeMap& types, diagnostics::DiagnosticSink& sink)
    : types_(types), sink_(sink) {}

void Emitter::write(std::string_view text) { out_.append(text); }

void Emitter::emit_expr(const ast::Expr& expr) { expr.accept(*this); }

void Emitter::refuse(const ast::Node& node, const std::string& what) {
    failed_ = true;
    sink_.report_error(diagnostic_code(EmitDiagnosticKind::UnsupportedConstruct),
                       what + " cannot be compiled to C++ yet", node.span().start_line,
                       node.span().start_column,
                       suppressibility_of(EmitDiagnosticKind::UnsupportedConstruct));
}

const semantic::Type* Emitter::type_of(const ast::Expr& expr) const {
    return types_.find(&expr);
}

std::optional<std::string> Emitter::emit_expression_for_test(const ast::Expr& expr) {
    out_.clear();
    failed_ = false;
    emit_expr(expr);
    if (failed_) {
        return std::nullopt;
    }
    return out_;
}

void Emitter::visit(const ast::Constant& node) {
    switch (node.type()) {
    case lexer::token_type::LITERAL_INT: {
        const std::string digits = without_underscores(node.lexeme());
        if (!is_decimal_digits(digits)) {
            // Hex, octal and binary literals spell differently in C++ (0o has
            // no C++ equivalent at all), so they are refused rather than
            // passed through and hoped for.
            refuse(node, "a non-decimal integer literal");
            return;
        }
        write("py::int_(");
        write(digits);
        write(")");
        return;
    }
    case lexer::token_type::LITERAL_FLOAT:
        write("py::float_(");
        write(without_underscores(node.lexeme()));
        write(")");
        return;
    case lexer::token_type::BOOL_TRUE:
        write("py::bool_(true)");
        return;
    case lexer::token_type::BOOL_FALSE:
        write("py::bool_(false)");
        return;
    case lexer::token_type::KEYWORD_NONE:
        write("py::none");
        return;
    case lexer::token_type::LITERAL_STRING: {
        const std::optional<std::string> bytes = decode_string_literal(node.lexeme());
        if (!bytes.has_value()) {
            refuse(node, "this string literal form");
            return;
        }
        write("py::str(std::string(\"");
        write(as_octal_escapes(*bytes));
        write("\", ");
        write(std::to_string(bytes->size()));
        write("))");
        return;
    }
    default:
        // A `default` here is correct and is NOT the -Werror=switch pattern
        // this project forbids: token_type is a large open vocabulary the
        // lexer owns, not a closed set this stage enumerates. Every literal
        // form this slice models is named above; anything else is refused.
        refuse(node, "this literal form");
        return;
    }
}

void Emitter::visit(const ast::Name& node) {
    if (!is_manglable_identifier(node.identifier())) {
        refuse(node, "a non-ASCII identifier");
        return;
    }
    // A name whose static type has no C++ representation (e.g. a `list[int]`
    // in this slice) is refused here rather than emitted: nothing downstream
    // would ever have declared a C++ variable for it, so writing the mangled
    // name out would reference something that does not exist.
    const semantic::Type* type = type_of(node);
    if (type == nullptr || !cpp_type_name(*type).has_value()) {
        refuse(node, "a name of this type");
        return;
    }
    write(mangle(node.identifier()));
}

// --- Expression nodes outside the slice -------------------------------------
// Each is refused by name rather than by a shared default, so the diagnostic
// tells the user which construct stopped the compile.

void Emitter::visit(const ast::Attribute& node) { refuse(node, "attribute access"); }
void Emitter::visit(const ast::DictExpr& node) { refuse(node, "a dict display"); }
void Emitter::visit(const ast::ListComp& node) { refuse(node, "a comprehension"); }
void Emitter::visit(const ast::ListExpr& node) { refuse(node, "a list display"); }
void Emitter::visit(const ast::Subscript& node) { refuse(node, "subscripting"); }
void Emitter::visit(const ast::TupleExpr& node) { refuse(node, "a tuple display"); }

// --- Filled in by Task 6 ----------------------------------------------------

void Emitter::visit(const ast::BinOp& node) { emit_binary(node); }
void Emitter::visit(const ast::BoolOp& node) { refuse(node, "a boolean operator"); }
void Emitter::visit(const ast::Call& node) { emit_call(node); }
void Emitter::visit(const ast::Compare& node) { refuse(node, "a comparison"); }
void Emitter::visit(const ast::UnaryOp& node) { refuse(node, "a unary operator"); }

void Emitter::emit_binary(const ast::BinOp& node) { refuse(node, "a binary operator"); }
void Emitter::emit_power(const ast::BinOp& node) { refuse(node, "the power operator"); }
void Emitter::emit_call(const ast::Call& node) { refuse(node, "a call"); }

} // namespace cythonpp::domain::codegen
