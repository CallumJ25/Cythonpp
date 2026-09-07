#include "domain/semantic/class_table.h"

#include <cstddef>
#include <utility>

#include "domain/semantic/builtin_class_table.h"
#include "domain/semantic/builtin_type_names.h"

namespace cythonpp::domain::semantic {

ClassTable::ClassTable() {
    for (std::size_t index = 0; index < kBuiltinClassCount; ++index) {
        const BuiltinClass& builtin = kBuiltinClasses[index];
        Entry& entry = classes_[builtin.name];
        for (const char* base : builtin.bases) {
            if (base != nullptr) {
                entry.bases.emplace_back(base);
            }
        }
    }
    for (std::size_t index = 0; index < kBuiltinClassAliasCount; ++index) {
        const BuiltinClassAlias& alias = kBuiltinClassAliases[index];
        aliases_.emplace(alias.alias, alias.canonical);
    }
}

std::string ClassTable::canonical_name(const std::string& name) const {
    const auto it = aliases_.find(name);
    return it == aliases_.end() ? name : it->second;
}

const ClassTable::Entry* ClassTable::find_entry(const std::string& name) const {
    const auto it = classes_.find(canonical_name(name));
    return it == classes_.end() ? nullptr : &it->second;
}

bool ClassTable::is_class(const std::string& name) const { return find_entry(name) != nullptr; }

std::vector<std::string> ClassTable::bases_of(const std::string& name) const {
    const Entry* entry = find_entry(name);
    return entry == nullptr ? std::vector<std::string>{} : entry->bases;
}

void ClassTable::declare(std::string qualified_name, std::vector<std::string> bases) {
    Entry& entry = classes_[std::move(qualified_name)];
    entry.bases = std::move(bases);
}

void ClassTable::declare_member(const std::string& qualified_name, std::string member, Type type,
                                 int declared_line) {
    Entry& entry = classes_[qualified_name];
    entry.members[std::move(member)] = Member{std::move(type), declared_line};
}

void ClassTable::declare_method(const std::string& qualified_name, std::string method,
                                 Type signature) {
    Entry& entry = classes_[qualified_name];
    entry.methods[std::move(method)] = std::move(signature);
}

std::optional<Type> ClassTable::member_type(const std::string& qualified_name,
                                             const std::string& member) const {
    std::vector<std::string> visited;
    return walk_chain<Type>(
        qualified_name, visited,
        [&member](const std::string&, const Entry& entry) -> std::optional<Type> {
            const auto it = entry.members.find(member);
            if (it == entry.members.end()) {
                return std::nullopt;
            }
            return it->second.type;
        });
}

std::optional<int> ClassTable::member_declared_line(const std::string& qualified_name,
                                                      const std::string& member) const {
    std::vector<std::string> visited;
    return walk_chain<int>(
        qualified_name, visited,
        [&member](const std::string&, const Entry& entry) -> std::optional<int> {
            const auto it = entry.members.find(member);
            if (it == entry.members.end()) {
                return std::nullopt;
            }
            return it->second.declared_line;
        });
}

Type ClassTable::constructor_type(const std::string& qualified_name) const {
    const std::string canonical = canonical_name(qualified_name);
    const Entry* entry = find_entry(canonical);

    std::vector<Type> params;
    if (entry != nullptr) {
        const auto it = entry->methods.find("__init__");
        if (it != entry->methods.end()) {
            const Type& init = it->second;
            // init.args is [self, param..., return], return last. Strip
            // both ends: self is not a caller-supplied argument, and the
            // return is replaced by the instance type below.
            for (std::size_t i = 1; i + 1 < init.args.size(); ++i) {
                params.push_back(init.args[i]);
            }
        }
    }
    return Type::callable(std::move(params), Type::class_of(canonical));
}

bool ClassTable::inherits_builtin(const std::string& qualified_name) const {
    std::vector<std::string> visited;
    const std::optional<bool> found = walk_chain<bool>(
        qualified_name, visited,
        [](const std::string& canonical, const Entry&) -> std::optional<bool> {
            // object is excluded deliberately: every class conceptually
            // derives from it, and is_subtype already treats Object as the
            // top of the lattice, so counting it here would make every
            // class "inherit a builtin" and delete the attr-defined check
            // entirely.
            if (canonical == "object") {
                return std::nullopt;
            }
            if (builtin_type_kind(canonical).has_value()) {
                return true;
            }
            return std::nullopt;
        });
    return found.has_value() && *found;
}

} // namespace cythonpp::domain::semantic
