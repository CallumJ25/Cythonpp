#include "annotation_resolver.h"

#include <array>
#include <string>
#include <utility>

#include "domain/ast/source_span.h"
#include "domain/lexer/token_type.h"

namespace cythonpp::domain::semantic {
namespace {

struct BuiltinTypeName {
    const char* spelling;
    TypeKind kind;
    // Type arguments the constructor takes: 0 for a non-generic builtin, 1
    // for list/set/frozenset, 2 for dict, and -1 for tuple, which takes any
    // number.
    int arity;
};

// Matched on the SPELLING rather than on token_type, deliberately. `int` in
// annotation position is TYPE_INT while the same word elsewhere is
// IDENTIFIER, and both parse to an ast::Name carrying the identifier -- so
// the string is the only thing always available.
//
// Fourteen entries: keyword_table.cpp's thirteen builtin type names, plus
// `range`. range is not one of the thirteen, so it arrives as an IDENTIFIER,
// but TypeKind::Range exists for what range() returns and `x: range` is legal
// Python, so omitting it would make a legal annotation a NameError.
constexpr std::array<BuiltinTypeName, 14> BUILTIN_TYPE_NAMES = {{
    {"bool", TypeKind::Bool, 0},
    {"bytearray", TypeKind::ByteArray, 0},
    {"bytes", TypeKind::Bytes, 0},
    {"complex", TypeKind::Complex, 0},
    {"dict", TypeKind::Dict, 2},
    {"float", TypeKind::Float, 0},
    {"frozenset", TypeKind::FrozenSet, 1},
    {"int", TypeKind::Int, 0},
    {"list", TypeKind::List, 1},
    {"object", TypeKind::Object, 0},
    {"range", TypeKind::Range, 0},
    {"set", TypeKind::Set, 1},
    {"str", TypeKind::Str, 0},
    {"tuple", TypeKind::Tuple, -1},
}};

const BuiltinTypeName* builtin_type_named(const std::string& spelling) {
    for (const BuiltinTypeName& entry : BUILTIN_TYPE_NAMES) {
        if (spelling == entry.spelling) {
            return &entry;
        }
    }
    return nullptr;
}

Type of_kind(TypeKind kind) {
    Type type;
    type.kind = kind;
    return type;
}

} // namespace

AnnotationResolver::AnnotationResolver(const ClassLookup& classes,
                                       diagnostics::DiagnosticSink& sink)
    : classes_(classes), sink_(sink) {}

Type AnnotationResolver::resolve(const ast::Expr& annotation) {
    // dynamic_cast rather than a Visitor, following assignability.cpp, which
    // asks this exact question six times. A Visitor would need all 26 methods
    // and would want the WRONG defaults: an unrecognised annotation shape is
    // an error, so recursing into it -- which is what RecursiveVisitor's
    // defaults do -- would descend into a Name and turn "not a valid type
    // annotation" into a confidently wrong type.
    if (const auto* name = dynamic_cast<const ast::Name*>(&annotation)) {
        return resolve_name(*name);
    }
    if (const auto* constant = dynamic_cast<const ast::Constant*>(&annotation)) {
        return resolve_constant(*constant);
    }
    // Task 10 adds the Subscript and BinOp arms here.
    return error(annotation, "TypeError", "not a valid type annotation");
}

Type AnnotationResolver::resolve_name(const ast::Name& name) {
    if (const BuiltinTypeName* builtin = builtin_type_named(name.identifier())) {
        if (builtin->arity != 0) {
            // mypy's type-arg rule under --strict's disallow-any-generics.
            return error(name, "TypeError",
                         "missing type parameters for generic type \"" + name.identifier() +
                             "\"");
        }
        return of_kind(builtin->kind);
    }
    if (classes_.is_class(name.identifier())) {
        return Type::class_of(name.identifier());
    }
    // A builtin that is not a type -- `x: print` -- also lands here. mypy
    // errors on it too, with different wording, so this is a wording
    // divergence rather than a compliance one.
    return error(name, "NameError", "name '" + name.identifier() + "' is not defined");
}

Type AnnotationResolver::resolve_constant(const ast::Constant& constant) {
    // `None` in annotation position lexes as KEYWORD_NONE and parses as a
    // Constant, NOT a Name -- so `x: str | None` is
    // (BinOp | (Name str) (Constant None)) and `-> None` is (Constant None).
    // Established by running the real binary. An implementation that handles
    // only Name in the union arms rejects every optional annotation in the
    // language.
    if (constant.type() == lexer::token_type::KEYWORD_NONE) {
        return Type::none();
    }
    if (constant.type() == lexer::token_type::LITERAL_STRING) {
        // mypy resolves these. Rejecting is allowed because an
        // unsupported-construct message is not a TypeError, and 5b's
        // two-pass collection already makes a plain Name forward reference
        // work -- which is the case forward references exist for.
        return error(constant, "NotImplementedError",
                     "string forward references are not supported");
    }
    return error(constant, "TypeError", "not a valid type annotation");
}

Type AnnotationResolver::error(const ast::Expr& at, std::string code, std::string message) {
    const ast::SourceSpan span = at.span();
    sink_.report_error(std::move(code), std::move(message), span.start_line, span.start_column);
    return Type::unknown();
}

} // namespace cythonpp::domain::semantic
