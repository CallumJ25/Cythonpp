#include "type_name.h"

#include <cstddef>
#include <string>
#include <vector>

namespace cythonpp::domain::semantic {
namespace {

std::string joined(const std::vector<Type>& types, const std::string& separator) {
    std::string result;
    for (std::size_t index = 0; index < types.size(); ++index) {
        if (index > 0) {
            result += separator;
        }
        result += type_name(types[index]);
    }
    return result;
}

// `name[args]`, or the bare name when there are no arguments -- unreachable
// through Type's factories, which always supply them, but type_name is total.
std::string parameterised(const std::string& base, const std::vector<Type>& args) {
    return args.empty() ? base : base + "[" + joined(args, ", ") + "]";
}

// mypy's error-message spelling: parameters bracketed as a list, then the
// return type. Note this is NOT reveal_type's `def (a: int) -> str` form,
// which is mypy's internal rendering and not what a diagnostic wants.
std::string callable_name(const std::vector<Type>& args) {
    if (args.empty()) {
        return "Callable";
    }
    const std::vector<Type> params(args.begin(), args.end() - 1);
    return "Callable[[" + joined(params, ", ") + "], " + type_name(args.back()) + "]";
}

} // namespace

std::string strip_synthetic_class_prefix(const std::string& name) {
    if (name.empty() || name.front() != '<') {
        return name;
    }
    const std::size_t close = name.find('>');
    if (close == std::string::npos || close + 1 >= name.size() || name[close + 1] != '#') {
        return name;
    }
    const std::size_t second_hash = name.find('#', close + 2);
    if (second_hash == std::string::npos) {
        return name;
    }
    return name.substr(second_hash + 1);
}

std::string type_name(const Type& type) {
    switch (type.kind) {
    case TypeKind::Unknown:
        return "Unknown";
    case TypeKind::NoneType:
        return "None";
    case TypeKind::Bool:
        return "bool";
    case TypeKind::Int:
        return "int";
    case TypeKind::Float:
        return "float";
    case TypeKind::Complex:
        return "complex";
    case TypeKind::Str:
        return "str";
    case TypeKind::Bytes:
        return "bytes";
    case TypeKind::ByteArray:
        return "bytearray";
    case TypeKind::Ellipsis:
        return "ellipsis";
    case TypeKind::List:
        return parameterised("list", type.args);
    case TypeKind::Dict:
        return parameterised("dict", type.args);
    case TypeKind::Set:
        return parameterised("set", type.args);
    case TypeKind::FrozenSet:
        return parameterised("frozenset", type.args);
    case TypeKind::Tuple:
        // mypy's spelling for the empty tuple type, verified: reveal_type(())
        // reports tuple[()].
        return type.args.empty() ? "tuple[()]" : parameterised("tuple", type.args);
    case TypeKind::Range:
        return "range";
    case TypeKind::Union:
        return joined(type.args, " | ");
    case TypeKind::Callable:
        return callable_name(type.args);
    case TypeKind::Class:
        // Strip TypeChecker's own internal isolation
        // prefix (see strip_synthetic_class_prefix's own comment) before
        // display -- an isolated class's `type.name` is a ClassTable KEY,
        // not something the user ever wrote.
        return strip_synthetic_class_prefix(type.name);
    case TypeKind::Object:
        return "object";
    }
    // Unreachable: the switch is exhaustive over TypeKind and has no default,
    // so adding a kind warns here rather than silently returning nothing.
    return "Unknown";
}

} // namespace cythonpp::domain::semantic
