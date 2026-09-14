#include "domain/codegen/cpp_type_name.h"

#include "domain/semantic/type_kind.h"

namespace cythonpp::domain::codegen {

std::optional<std::string> cpp_type_name(const semantic::Type& type) {
    // Exhaustive with no `default` label, and CMakeLists.txt passes
    // -Werror=switch, so a new TypeKind is a compile error here rather than
    // silently receiving nullopt. That matters: nullopt means "refuse", and a
    // new kind silently defaulting to refusal would be a capability
    // regression nobody was told about.
    switch (type.kind) {
    case semantic::TypeKind::Int:
        return "py::int_";
    case semantic::TypeKind::Float:
        return "py::float_";
    case semantic::TypeKind::Bool:
        return "py::bool_";
    case semantic::TypeKind::Str:
        return "py::str";
    case semantic::TypeKind::NoneType:
        return "py::none_t";

    // Everything below is outside this slice. Listed individually rather
    // than defaulted, so adding a kind forces a decision.
    case semantic::TypeKind::Unknown:
    case semantic::TypeKind::Complex:
    case semantic::TypeKind::Bytes:
    case semantic::TypeKind::ByteArray:
    case semantic::TypeKind::Ellipsis:
    case semantic::TypeKind::List:
    case semantic::TypeKind::Dict:
    case semantic::TypeKind::Set:
    case semantic::TypeKind::FrozenSet:
    case semantic::TypeKind::Tuple:
    case semantic::TypeKind::Range:
    case semantic::TypeKind::Union:
    case semantic::TypeKind::Callable:
    case semantic::TypeKind::Class:
    case semantic::TypeKind::Object:
        return std::nullopt;
    }
    return std::nullopt;
}

} // namespace cythonpp::domain::codegen
