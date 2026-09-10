#include "annotation_resolver.h"

#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "builtin_type_names.h"
#include "domain/ast/source_span.h"
#include "domain/ast/tuple_expr.h"
#include "domain/lexer/token_type.h"

namespace cythonpp::domain::semantic {
namespace {

Type of_kind(TypeKind kind) {
    Type type;
    type.kind = kind;
    return type;
}

// The seven builtin classes typeshed marks generic -- zip, map, filter,
// enumerate, reversed, staticmethod, classmethod. Bare use (`x: zip`) is a
// mypy type-arg error while ours is clean (a recorded direction-(b) miss),
// but `x: zip[int]` is mypy-CLEAN, so subscripting one of these must draw
// NotImplementedError rather than the TypeError a genuinely non-generic
// seeded class (e.g. `x: ValueError[int]`, a real mypy type-arg error) or a
// genuinely user-defined generic (`class C: pass` then `x: C[int]`) draws.
//
// Deliberately narrower than "is this name in kBuiltinClasses" -- this
// predicate's predecessor, is_seeded_builtin_class, matched the whole
// 97-entry table and so reported EVERY subscripted seeded class as
// unimplemented, even a non-generic one where mypy gives a genuine TypeError.
// No missed error resulted (a diagnostic still fired either way) but the
// wrong ONE fired. ClassLookup deliberately has only is_class and bases_of --
// 5a's header explains the two-method choice -- so this cannot be answered by
// asking `classes_`; consulting a small local list here, in the one place
// that needs the distinction, keeps that seam intact.
bool is_generic_builtin_class(const std::string& name) {
    static constexpr const char* kGenericBuiltinClasses[] = {
        "zip", "map", "filter", "enumerate", "reversed", "staticmethod", "classmethod",
    };
    for (const char* generic : kGenericBuiltinClasses) {
        if (name == generic) {
            return true;
        }
    }
    return false;
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
    return error(annotation, DiagnosticKind::SemanticAnalyzerTypeError,
                 "not a valid type annotation");
}

Type AnnotationResolver::resolve_name(const ast::Name& name) {
    if (std::optional<TypeKind> kind = builtin_type_kind(name.identifier())) {
        if (builtin_type_arity(name.identifier()) != 0) {
            // mypy's type-arg rule under --strict's disallow-any-generics.
            return error(name, DiagnosticKind::SemanticAnalyzerTypeError,
                         "missing type parameters for generic type \"" + name.identifier() +
                             "\"");
        }
        return of_kind(*kind);
    }
    if (classes_.is_class(name.identifier())) {
        // Not Type::class_of(name.identifier()) unconditionally:
        // EnvironmentError, IOError and WindowsError are the SAME class
        // object as OSError, not three distinct classes with a shared base,
        // so the Type built here must be spelled with the canonical name --
        // otherwise Class("IOError") and Class("OSError") would compare
        // unequal despite being one class. Routed through classes_ itself
        // (ClassLookup::canonical_name), not a second, file-local alias
        // table: that duplication is exactly what let this resolver disagree
        // with ClassTable's own precedence (a user class declared under the
        // alias spelling, e.g. `class IOError: pass`, must resolve to
        // itself, and only ClassTable's canonical_name knows that).
        return Type::class_of(classes_.canonical_name(name.identifier()));
    }
    // A builtin that is not a type -- `x: print` -- also lands here. mypy
    // errors on it too, with different wording, so this is a wording
    // divergence rather than a compliance one.
    return error(name, DiagnosticKind::NameError,
                 "name '" + name.identifier() + "' is not defined");
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
        return error(constant, DiagnosticKind::NotImplementedError,
                     "string forward references are not supported");
    }
    return error(constant, DiagnosticKind::SemanticAnalyzerTypeError,
                 "not a valid type annotation");
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
        return error(attribute, DiagnosticKind::SemanticAnalyzerTypeError,
                     "not a valid type annotation");
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
    return error(attribute, DiagnosticKind::NameError, "name '" + dotted + "' is not defined");
}

Type AnnotationResolver::resolve_union(const ast::BinOp& operation) {
    // PEP 604. Only `|` builds a union; every other binary operator in
    // annotation position is simply not an annotation.
    if (operation.op() != lexer::token_type::OP_PIPE) {
        return error(operation, DiagnosticKind::SemanticAnalyzerTypeError,
                     "not a valid type annotation");
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
        return error(subscript, DiagnosticKind::SemanticAnalyzerTypeError,
                     "not a valid type annotation");
    }

    std::optional<TypeKind> kind = builtin_type_kind(base->identifier());
    if (!kind.has_value()) {
        if (classes_.is_class(base->identifier())) {
            if (is_generic_builtin_class(base->identifier())) {
                // `x: zip[int]` is mypy-CLEAN -- zip is generic in typeshed --
                // so this must not be the same TypeError a genuinely
                // non-generic user class draws below. Subscripting a builtin
                // generic is simply unimplemented, not a type error on a
                // clean program.
                return error(*base, DiagnosticKind::NotImplementedError,
                             "generic builtin type '" + base->identifier() + "' is not supported");
            }
            // Either a genuine user class (no user-defined generics in the
            // subset) or a seeded builtin that is NOT one of the seven
            // generic ones -- `x: ValueError[int]` is a real mypy type-arg
            // error too, so this is the correct diagnostic for it as well.
            return error(*base, DiagnosticKind::SemanticAnalyzerTypeError,
                         "'" + base->identifier() + "' is not subscriptable");
        }
        // Optional, Union, Callable and Generic all land here: none of them
        // can be imported, so the base is simply undefined. This is where the
        // import gap becomes visible, and it is the correct outcome.
        return error(*base, DiagnosticKind::NameError,
                     "name '" + base->identifier() + "' is not defined");
    }
    const int arity = builtin_type_arity(base->identifier());
    if (arity == 0) {
        return error(*base, DiagnosticKind::SemanticAnalyzerTypeError,
                     "'" + base->identifier() + "' is not subscriptable");
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
    if (*kind == TypeKind::Tuple) {
        for (const ast::Expr* argument : arguments) {
            const auto* constant = dynamic_cast<const ast::Constant*>(argument);
            if (constant != nullptr && constant->type() == lexer::token_type::ELLIPSIS) {
                // mypy accepts tuple[int, ...], so this must NOT be a
                // TypeError or the hard invariant breaks.
                return error(*argument, DiagnosticKind::NotImplementedError,
                             "variadic tuple annotations are not supported");
            }
        }
    }

    if (arity > 0 && arguments.size() != static_cast<std::size_t>(arity)) {
        // Before resolving the arguments, so a wrong-arity annotation draws
        // one diagnostic about arity rather than that plus one per argument.
        const std::string plural = arity == 1 ? " type argument, but " : " type arguments, but ";
        return error(*base, DiagnosticKind::SemanticAnalyzerTypeError,
                     "\"" + base->identifier() + "\" expects " + std::to_string(arity) + plural +
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
    type.kind = *kind;
    type.args = std::move(resolved);
    return type;
}

Type AnnotationResolver::error(const ast::Expr& at, DiagnosticKind kind, std::string message) {
    const ast::SourceSpan span = at.span();
    sink_.report_error(diagnostic_code(kind), std::move(message), span.start_line,
                       span.start_column, suppressibility_of(kind));
    return Type::unknown();
}

} // namespace cythonpp::domain::semantic
