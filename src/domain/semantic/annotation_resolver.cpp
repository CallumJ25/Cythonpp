#include "annotation_resolver.h"

#include <array>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "domain/ast/source_span.h"
#include "domain/ast/tuple_expr.h"
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
    if (const auto* attribute = dynamic_cast<const ast::Attribute*>(&annotation)) {
        return resolve_attribute(*attribute);
    }
    if (const auto* subscript = dynamic_cast<const ast::Subscript*>(&annotation)) {
        return resolve_subscript(*subscript);
    }
    if (const auto* operation = dynamic_cast<const ast::BinOp*>(&annotation)) {
        return resolve_union(*operation);
    }
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
    //
    // RECORDED OBLIGATION (not fixed here, ruled out of Spec 5a): the
    // reasoning above only covers names mypy itself rejects. It does NOT
    // cover builtin classes outside BUILTIN_TYPE_NAMES' 14 entries --
    // `x: type`, `x: Exception`, `x: BaseException`, `x: slice`,
    // `x: memoryview` are all mypy --strict clean, yet draw this same
    // NameError, a genuine false positive against invariant (a). 5a's
    // behaviour is arguably already correct given what exists today (no real
    // ClassLookup implementation is wired in yet), but nothing recorded the
    // obligation until now: Spec 5b's class table is expected to close this
    // by seeding itself with builtin class names so is_class(...) picks them
    // up here before falling through.
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

Type AnnotationResolver::resolve_attribute(const ast::Attribute& attribute) {
    // Attribute chains nest LEFTWARD: `A.B.D` parses as
    // (Attribute (Attribute (Name A) B) D), so walking `.value()` down to the
    // root and collecting `.attribute()` on the way yields the segments in
    // reverse order -- reversed back below to read left to right.
    std::vector<std::string> segments;
    const ast::Expr* cursor = &attribute;
    while (const auto* link = dynamic_cast<const ast::Attribute*>(cursor)) {
        segments.push_back(link->attribute());
        cursor = &link->value();
    }

    const auto* root = dynamic_cast<const ast::Name*>(cursor);
    if (root == nullptr) {
        // e.g. `f().x`: mypy rejects this too ("Invalid type comment or
        // annotation"), so TypeError is correct here, not a compliance gap.
        return error(attribute, "TypeError", "not a valid type annotation");
    }

    std::string dotted = root->identifier();
    for (auto it = segments.rbegin(); it != segments.rend(); ++it) {
        dotted += "." + *it;
    }

    if (classes_.is_class(dotted)) {
        return Type::class_of(dotted);
    }
    // A qualified name mypy would resolve to a real class is registered under
    // that same dotted name; if it is not in the table, mypy would not have
    // resolved it either, so this cannot make invariant (a) false.
    return error(attribute, "NameError", "name '" + dotted + "' is not defined");
}

Type AnnotationResolver::resolve_union(const ast::BinOp& operation) {
    // PEP 604. Only `|` builds a union; every other binary operator in
    // annotation position is simply not an annotation.
    if (operation.op() != lexer::token_type::OP_PIPE) {
        return error(operation, "TypeError", "not a valid type annotation");
    }

    const Type left = resolve(operation.left());
    const Type right = resolve(operation.right());
    // Absorbing rather than a union carrying an Unknown member: the failing
    // side already reported, and such a union would compare true against
    // everything while rendering nonsense.
    if (left.kind == TypeKind::Unknown || right.kind == TypeKind::Unknown) {
        return Type::unknown();
    }
    // union_of flattens, so `int | str | None` -- which parses as
    // BinOp(|, BinOp(|, int, str), None) -- is one three-member union.
    return Type::union_of({left, right});
}

Type AnnotationResolver::resolve_subscript(const ast::Subscript& subscript) {
    const auto* base = dynamic_cast<const ast::Name*>(&subscript.value());
    if (base == nullptr) {
        return error(subscript, "TypeError", "not a valid type annotation");
    }

    const BuiltinTypeName* builtin = builtin_type_named(base->identifier());
    if (builtin == nullptr) {
        if (classes_.is_class(base->identifier())) {
            // No user-defined generics in the subset.
            return error(*base, "TypeError",
                         "'" + base->identifier() + "' is not subscriptable");
        }
        // Optional, Union, Callable and Generic all land here: none of them
        // can be imported, so the base is simply undefined. This is where the
        // import gap becomes visible, and it is the correct outcome.
        return error(*base, "NameError", "name '" + base->identifier() + "' is not defined");
    }
    if (builtin->arity == 0) {
        return error(*base, "TypeError", "'" + base->identifier() + "' is not subscriptable");
    }

    // dict[str, int] arrives as Subscript(Name dict, TupleExpr(str, int));
    // list[int] as Subscript(Name list, Name int). Flattening the one-element
    // case here means the arity check below is the same for both.
    std::vector<const ast::Expr*> arguments;
    if (const auto* tuple = dynamic_cast<const ast::TupleExpr*>(&subscript.index())) {
        for (const ast::ExprPtr& element : tuple->elements()) {
            arguments.push_back(element.get());
        }
    } else {
        arguments.push_back(&subscript.index());
    }

    // Only for tuple, and BEFORE the arity check: tuple is variadic, so arity
    // would never catch tuple[int, ...]. Restricting it to tuple means
    // list[int, ...] gets the arity message instead of a misleading one.
    if (builtin->kind == TypeKind::Tuple) {
        for (const ast::Expr* argument : arguments) {
            const auto* constant = dynamic_cast<const ast::Constant*>(argument);
            if (constant != nullptr && constant->type() == lexer::token_type::ELLIPSIS) {
                // mypy accepts tuple[int, ...], so this must NOT be a
                // TypeError or the hard invariant breaks.
                return error(*argument, "NotImplementedError",
                             "variadic tuple annotations are not supported");
            }
        }
    }

    if (builtin->arity > 0 && arguments.size() != static_cast<std::size_t>(builtin->arity)) {
        // Before resolving the arguments, so a wrong-arity annotation draws
        // one diagnostic about arity rather than that plus one per argument.
        const std::string plural = builtin->arity == 1 ? " type argument, but "
                                                       : " type arguments, but ";
        return error(*base, "TypeError",
                     "\"" + base->identifier() + "\" expects " +
                         std::to_string(builtin->arity) + plural +
                         std::to_string(arguments.size()) + " given");
    }

    std::vector<Type> resolved;
    bool failed = false;
    for (const ast::Expr* argument : arguments) {
        Type type = resolve(*argument);
        // Every argument is resolved even after one fails, so two undefined
        // names draw two diagnostics -- two root causes -- as mypy's do.
        if (type.kind == TypeKind::Unknown) {
            failed = true;
        }
        resolved.push_back(std::move(type));
    }
    if (failed) {
        // Silently: the failing argument already reported, and a second
        // report from the enclosing constructor is exactly the cascade the
        // absorbing bottom exists to prevent.
        return Type::unknown();
    }

    Type type;
    type.kind = builtin->kind;
    type.args = std::move(resolved);
    return type;
}

Type AnnotationResolver::error(const ast::Expr& at, std::string code, std::string message) {
    const ast::SourceSpan span = at.span();
    sink_.report_error(std::move(code), std::move(message), span.start_line, span.start_column);
    return Type::unknown();
}

} // namespace cythonpp::domain::semantic
