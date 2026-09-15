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

#include <array>
#include <cstddef>

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

// The runtime function for a binary operator, or nullptr outside the slice.
const char* binary_runtime_function(lexer::token_type op) {
    switch (op) {
    case lexer::token_type::OP_PLUS: return "py::add";
    case lexer::token_type::OP_MINUS: return "py::sub";
    case lexer::token_type::OP_STAR: return "py::mul";
    case lexer::token_type::OP_SLASH: return "py::truediv";
    case lexer::token_type::OP_DOUBLE_SLASH: return "py::floordiv";
    case lexer::token_type::OP_PERCENT: return "py::mod";
    default: return nullptr;
    }
}

// NOTE: the task brief spelled the `==` case OP_EQUAL_EQUAL. The real
// enumerator in domain/lexer/token_type.h is OP_EQUAL (OP_NOT_EQUAL is the
// only one of the six comparison operators whose name doubles a word); this
// switch uses the header's actual spelling.
const char* comparison_runtime_function(lexer::token_type op) {
    switch (op) {
    case lexer::token_type::OP_LESS: return "py::lt";
    case lexer::token_type::OP_LESS_EQUAL: return "py::le";
    case lexer::token_type::OP_GREATER: return "py::gt";
    case lexer::token_type::OP_GREATER_EQUAL: return "py::ge";
    case lexer::token_type::OP_EQUAL: return "py::eq";
    case lexer::token_type::OP_NOT_EQUAL: return "py::ne";
    default: return nullptr;
    }
}

// The builtin names src/domain/semantic/builtin_call_table.cpp MODELS (its
// own kSupportedBuiltinCalls, is_supported_builtin_call's backing set),
// mirrored here minus "print" and "len" -- the two this slice actually
// emits. Every other name in that list is a real builtin this compiler
// understands well enough to TYPE but has no runtime/codegen support for
// yet; falling through to mangle() for one of them would silently emit a
// call to a C++ function that does not exist (`cy_abs(...)`), a worse
// failure than an honest refusal naming the builtin.
//
// Deliberately NOT widened to every name is_builtin_callable_name accepts
// (the generated builtin-function table, plus enumerate/zip/map/filter/
// reversed): the task that authored this list scoped it to the "modelled"
// set builtin_call_table.cpp itself defines that way, and a name outside it
// (`hash`, `enumerate`, ...) reaching mangle() unrefused is a known,
// narrower residual gap left for a future round rather than silently
// expanded scope here.
constexpr std::array<const char*, 25> kRefusedBuiltinCalls = {{
    "range", "abs",  "int",       "float", "str",    "bool",  "bytes",
    "bytearray", "round", "ord",  "chr",   "hex",    "repr",  "input",
    "divmod", "sum", "min",       "max",   "sorted", "isinstance",
    "list", "dict", "set", "frozenset", "tuple",
}};

bool is_refused_builtin_call(const std::string& name) {
    for (const char* candidate : kRefusedBuiltinCalls) {
        if (name == candidate) {
            return true;
        }
    }
    return false;
}

// FIX (review round 1, gap found beyond the two originally reported): the
// numeric tower (Bool, Int, Float) shares raw types (bool, int64_t, double)
// that ordinary C++ comparison operators freely convert between; py::str's
// raw type (std::string) compares only with itself; py::none_t has no raw()
// at all. py::lt/le/gt/ge/eq/ne (compare.h) are templates over `.raw()`, so
// a comparison between two operands outside one of those two groups has no
// working instantiation.
//
// <,<=,>,>= already restrict TypeChecker's accepted combinations to "both
// numeric" or "exactly matching kind" (operator_rules.cpp's ordered_result),
// so within this slice's five in-slice scalar kinds this predicate can never
// refuse a <,<=,>,>= pair that would otherwise have compiled. == and != are
// different: operator_rules.h states plainly that they are TOTAL ("any
// operands, always bool, never a report" -- mypy's strict-equality opt-in
// check is explicitly out of scope), so `"x" == 1` and `1 == None` both
// type-check clean yet have no matching runtime instantiation: std::string
// has no operator== against bool/int64_t/double, and none_t has no raw() to
// call at all. This predicate is checked per adjacent pair in visit(Compare)
// so it fires only where an actual runtime call would fail to compile.
bool is_numeric_tower_kind(semantic::TypeKind kind) {
    return kind == semantic::TypeKind::Bool || kind == semantic::TypeKind::Int ||
           kind == semantic::TypeKind::Float;
}

bool comparison_operands_are_runtime_comparable(const semantic::Type* left,
                                                const semantic::Type* right) {
    if (left == nullptr || right == nullptr) {
        return false;
    }
    if (is_numeric_tower_kind(left->kind) && is_numeric_tower_kind(right->kind)) {
        return true;
    }
    return left->kind == semantic::TypeKind::Str && right->kind == semantic::TypeKind::Str;
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
void Emitter::visit(const ast::Call& node) { emit_call(node); }

void Emitter::visit(const ast::UnaryOp& node) {
    switch (node.op()) {
    case lexer::token_type::OP_MINUS:
        write("py::neg(");
        emit_expr(node.operand());
        write(")");
        return;
    case lexer::token_type::OP_PLUS:
        // Python's unary plus on a number is the identity, so it emits
        // nothing of its own rather than a no-op runtime call.
        emit_expr(node.operand());
        return;
    case lexer::token_type::OP_NOT:
        write("py::not_(");
        emit_expr(node.operand());
        write(")");
        return;
    default:
        refuse(node, "this unary operator");
        return;
    }
}

// A chain evaluates each operand EXACTLY ONCE -- `f() < x < g()` calls f and
// g once each -- so it cannot desugar to `a < b && b < c`, which would
// evaluate b twice. An immediately-invoked lambda binds each operand to a
// temporary and short-circuits between them.
//
// auto&& is safe for a temporary: binding one to an rvalue reference extends
// its lifetime to the end of the reference's scope, which here is the lambda
// body.
void Emitter::visit(const ast::Compare& node) {
    if (node.rest().empty()) {
        refuse(node, "a comparison with no operator");
        return;
    }
    // See comparison_operands_are_runtime_comparable's own comment: == and !=
    // are total in operator_rules.h, so a pair like `"x" == 1` or `1 ==
    // None` type-checks clean but has no matching py::eq/ne instantiation.
    // Checked per ADJACENT pair, matching exactly what gets emitted below --
    // a chain runs op[i] between operand[i] and operand[i+1], never all
    // pairs.
    {
        const ast::Expr* previous_operand = &node.left();
        for (const ast::Compare::Rest& link : node.rest()) {
            if (!comparison_operands_are_runtime_comparable(type_of(*previous_operand),
                                                             type_of(*link.operand))) {
                refuse(node, "a comparison between these operand types");
                return;
            }
            previous_operand = link.operand.get();
        }
    }
    if (node.rest().size() == 1) {
        const char* function = comparison_runtime_function(node.rest().front().op);
        if (function == nullptr) {
            refuse(node, "this comparison operator");
            return;
        }
        write(function);
        write("(");
        emit_expr(node.left());
        write(", ");
        emit_expr(*node.rest().front().operand);
        write(")");
        return;
    }
    for (const ast::Compare::Rest& link : node.rest()) {
        if (comparison_runtime_function(link.op) == nullptr) {
            refuse(node, "this comparison operator");
            return;
        }
    }
    write("([&]() -> py::bool_ { auto&& _cy_cmp_0 = (");
    emit_expr(node.left());
    write("); ");
    std::size_t index = 0;
    for (const ast::Compare::Rest& link : node.rest()) {
        const std::string current = "_cy_cmp_" + std::to_string(index + 1);
        write("auto&& ");
        write(current);
        write(" = (");
        emit_expr(*link.operand);
        write("); ");
        const std::string previous = "_cy_cmp_" + std::to_string(index);
        const bool last = index + 1 == node.rest().size();
        if (last) {
            write("return ");
        } else {
            write("if (!py::truthy(");
        }
        write(comparison_runtime_function(link.op));
        write("(");
        write(previous);
        write(", ");
        write(current);
        write(")");
        if (last) {
            write("; ");
        } else {
            write(")) { return py::bool_(false); } ");
        }
        ++index;
    }
    write("}())");
}

// `and` and `or` return an OPERAND, not a bool, and short-circuit. The right
// side is passed as a lambda so it is evaluated at most once and only when
// needed.
//
// Differing operand types make the expression's own type a Union, which
// cpp_type_name maps to nullopt -- so that case is refused by the check
// below with no rule of its own.
void Emitter::visit(const ast::BoolOp& node) {
    const char* function = nullptr;
    switch (node.op()) {
    case lexer::token_type::OP_AND: function = "py::and_"; break;
    case lexer::token_type::OP_OR: function = "py::or_"; break;
    default:
        refuse(node, "this boolean operator");
        return;
    }
    const semantic::Type* result = type_of(node);
    if (result == nullptr || !cpp_type_name(*result).has_value()) {
        refuse(node, "a boolean operator over differing operand types");
        return;
    }
    if (node.values().size() != 2) {
        refuse(node, "a boolean operator with more than two operands");
        return;
    }
    write(function);
    write("(");
    emit_expr(*node.values().front());
    write(", [&]{ return ");
    emit_expr(*node.values().back());
    write("; })");
}

void Emitter::emit_binary(const ast::BinOp& node) {
    if (node.op() == lexer::token_type::OP_DOUBLE_STAR) {
        emit_power(node);
        return;
    }
    const char* function = binary_runtime_function(node.op());
    if (function == nullptr) {
        refuse(node, "this binary operator");
        return;
    }
    const semantic::Type* result = type_of(node);
    if (result == nullptr || !cpp_type_name(*result).has_value()) {
        refuse(node, "an operand type");
        return;
    }
    // FIX (review round 1): `%` on a Str left operand is printf-style string
    // formatting. operator_rules.cpp's modulo_result types `str % anything`
    // as Str -- a genuine, mypy-agreeing type judgement, not a modelling
    // mistake -- so the check above alone lets it through (cpp_type_name(Str)
    // is "py::str", a valid mapping). But py::mod (int_.h, float_.h) has no
    // overload taking a str at all, so `"x" % 1` used to emit
    // py::mod(py::str(...), py::int_(1)), which does not compile. Printf-
    // style formatting is genuinely outside this slice (the spec puts
    // str-format checking out of scope entirely), so this refuses rather
    // than guessing at a runtime implementation. Gated on the LEFT operand
    // only, matching modulo_result's own asymmetry: the right operand's kind
    // never changes str's printf-style verdict.
    if (node.op() == lexer::token_type::OP_PERCENT) {
        const semantic::Type* left_type = type_of(node.left());
        if (left_type != nullptr && left_type->kind == semantic::TypeKind::Str) {
            refuse(node, "printf-style string formatting");
            return;
        }
    }
    write(function);
    write("(");
    emit_operand_widened(node.left(), *result);
    write(", ");
    emit_operand_widened(node.right(), *result);
    write(")");
}

// Python's bool is an int subtype, so `True + 1` is `2`. The runtime has no
// bool arithmetic overloads -- adding eight of them would be the alternative
// -- so the widening is inserted here, and only where the operation's own
// result is not itself a bool.
void Emitter::emit_operand_widened(const ast::Expr& operand, const semantic::Type& result) {
    const semantic::Type* operand_type = type_of(operand);
    const bool widen = operand_type != nullptr
                       && operand_type->kind == semantic::TypeKind::Bool
                       && result.kind != semantic::TypeKind::Bool;
    if (widen) {
        write("py::to_int(");
    }
    emit_expr(operand);
    if (widen) {
        write(")");
    }
}

// SPEC DECISION 4a. The gate is on the exponent's AST SHAPE, never on its
// type, and that distinction is the whole point:
//
//   * A negative literal parses as UnaryOp(-, Constant), so testing for a
//     bare Constant excludes it. The sign lives in a node the TYPE cannot
//     see -- the same fact literal_type.h records for the 64-bit magnitude
//     predicate.
//   * The TypeMap is not merely imprecise here, it is WRONG. Measured
//     2026-09-14: cythonpp types `2 ** -1` as int where the value is 0.5,
//     and types `(-8.0) ** 0.5` as float where both CPython and mypy say
//     complex. A gate reading the type would emit confidently wrong numbers.
//   * mypy itself types a non-literal exponent as `Any`. There is no honest
//     C++ type to emit for one, so refusing matches the more precise oracle
//     rather than giving up early.
//
// If you are tempted to "improve" this by consulting type_of(node.right()),
// the test PowerWithANegativeLiteralExponentIsRefused exists to stop you.
void Emitter::emit_power(const ast::BinOp& node) {
    const auto* exponent = dynamic_cast<const ast::Constant*>(&node.right());
    if (exponent == nullptr || exponent->type() != lexer::token_type::LITERAL_INT) {
        refuse(node, "a power whose exponent is not a non-negative integer literal");
        return;
    }
    const semantic::Type* base = type_of(node.left());
    if (base == nullptr || !cpp_type_name(*base).has_value()) {
        refuse(node, "a power base type");
        return;
    }
    // FIX (review round 1): the whole expression's result -- never Bool,
    // since binary_result floors `**` at numeric_join's minimum rank, Int --
    // is what emit_operand_widened needs to decide whether the BASE needs
    // py::to_int wrapping. A Bool base (`True ** 2`) used to reach here with
    // no widening at all: cpp_type_name(Bool) is "py::bool_", a valid
    // mapping, so the `base` check above passed, and emit_expr(node.left())
    // wrote a bare py::bool_(true) straight into py::pow(...) -- which has
    // exactly two overloads, pow(int_, int_) and pow(float_, int_), and
    // bool_'s constructor is explicit with no conversion operator, so that
    // does not compile. Checking `result` (rather than reusing `base`) and
    // routing the base through emit_operand_widened closes this the same way
    // emit_binary already does for its own two operands.
    const semantic::Type* result = type_of(node);
    if (result == nullptr || !cpp_type_name(*result).has_value()) {
        refuse(node, "a power result type");
        return;
    }
    write("py::pow(");
    emit_operand_widened(node.left(), *result);
    write(", ");
    emit_operand_widened(node.right(), *result);
    write(")");
}

void Emitter::emit_call(const ast::Call& node) {
    const auto* callee = dynamic_cast<const ast::Name*>(&node.callee());
    if (callee == nullptr) {
        refuse(node, "a call to something other than a plain name");
        return;
    }
    const std::string& name = callee->identifier();

    // The two builtins this slice models. Every other name in
    // builtin_call_table.cpp's modelled set (kRefusedBuiltinCalls above) is
    // refused explicitly by name, so the diagnostic says which one, rather
    // than falling through to mangle() and emitting a call to a C++ function
    // that does not exist.
    if (name == "print" || name == "len") {
        write(name == "print" ? "py::print" : "py::len");
    } else if (is_refused_builtin_call(name)) {
        refuse(node, "the builtin '" + name + "'");
        return;
    } else {
        if (!is_manglable_identifier(name)) {
            refuse(node, "a non-ASCII identifier");
            return;
        }
        write(mangle(name));
    }

    write("(");
    bool first = true;
    for (const ast::ExprPtr& argument : node.args()) {
        if (!first) {
            write(", ");
        }
        first = false;
        emit_expr(*argument);
    }
    write(")");
}

} // namespace cythonpp::domain::codegen
