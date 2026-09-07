#include "builtin_type_names.h"

#include <array>

namespace cythonpp::domain::semantic {
namespace {

struct BuiltinTypeName {
    const char* spelling;
    TypeKind kind;
    // Type arguments the constructor takes: 0 for a non-generic builtin, 1
    // for list/set/frozenset, 2 for dict, and kVariadicArity for tuple, which
    // takes any number.
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
    {"tuple", TypeKind::Tuple, kVariadicArity},
}};

const BuiltinTypeName* builtin_type_named(const std::string& spelling) {
    for (const BuiltinTypeName& entry : BUILTIN_TYPE_NAMES) {
        if (spelling == entry.spelling) {
            return &entry;
        }
    }
    return nullptr;
}

} // namespace

std::optional<TypeKind> builtin_type_kind(const std::string& name) {
    const BuiltinTypeName* entry = builtin_type_named(name);
    if (entry == nullptr) {
        return std::nullopt;
    }
    return entry->kind;
}

int builtin_type_arity(const std::string& name) {
    const BuiltinTypeName* entry = builtin_type_named(name);
    if (entry == nullptr) {
        return 0;
    }
    return entry->arity;
}

} // namespace cythonpp::domain::semantic
