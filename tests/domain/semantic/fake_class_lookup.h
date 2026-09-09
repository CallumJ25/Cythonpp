#ifndef CYTHONPP_TESTS_DOMAIN_SEMANTIC_FAKE_CLASS_LOOKUP_H
#define CYTHONPP_TESTS_DOMAIN_SEMANTIC_FAKE_CLASS_LOOKUP_H

#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "domain/semantic/builtin_type_names.h"
#include "domain/semantic/class_lookup.h"
#include "domain/semantic/type.h"
#include "domain/semantic/type_compatibility.h"

namespace cythonpp::domain::semantic {
namespace semantic_test_support {

// A ClassLookup built from a literal map of class name to direct bases,
// each base still spelled as a bare source-level NAME (the constructor
// signature every test in this tree already writes literals against).
//
// Deliberately does not validate the table: a base naming a class that is not
// itself declared, or a cycle, are both constructible on purpose, because
// is_subtype must survive a malformed table and only a fake can hand it one.
class FakeClassLookup : public ClassLookup {
public:
    FakeClassLookup() = default;
    explicit FakeClassLookup(std::map<std::string, std::vector<std::string>> classes)
        : classes_(to_typed(std::move(classes))) {}

    bool is_class(const std::string& name) const override {
        return classes_.find(name) != classes_.end();
    }

    std::vector<Type> bases_of(const std::string& name) const override {
        const auto found = classes_.find(name);
        return found == classes_.end() ? std::vector<Type>() : found->second;
    }

    // Identity. This fake has no alias table of its own -- every test built
    // on it names its classes directly, and the real builtin alias mapping
    // (EnvironmentError/IOError/WindowsError -> OSError) only exists on
    // ClassTable. A test that needs to exercise actual canonicalisation uses
    // the real ClassTable instead (see class_table_test.cpp and the
    // ClassTable-based tests in annotation_resolver_test.cpp /
    // type_compatibility_test.cpp).
    std::string canonical_name(const std::string& name) const override { return name; }

private:
    // Converts each base's bare source-level NAME to a Type, the same
    // classification ClassTable's own seeding and TypeChecker::base_types
    // both perform: a name this model represents as a builtin KIND becomes
    // that kind's bare Type; anything else is an ordinary Class. Kept here,
    // at construction time, rather than pushed onto every call site, so every
    // existing FakeClassLookup({...}) literal across the semantic tests keeps
    // naming bases as plain strings -- this constructor's signature is
    // unchanged.
    static std::map<std::string, std::vector<Type>> to_typed(
        std::map<std::string, std::vector<std::string>> classes) {
        std::map<std::string, std::vector<Type>> typed;
        for (auto& [name, bases] : classes) {
            std::vector<Type> types;
            for (const std::string& base : bases) {
                const std::optional<TypeKind> kind = builtin_type_kind(base);
                types.push_back(kind.has_value() ? builtin_base_type(*kind)
                                                 : Type::class_of(base));
            }
            typed.emplace(name, std::move(types));
        }
        return typed;
    }

    std::map<std::string, std::vector<Type>> classes_;
};

} // namespace semantic_test_support
} // namespace cythonpp::domain::semantic

#endif // CYTHONPP_TESTS_DOMAIN_SEMANTIC_FAKE_CLASS_LOOKUP_H
