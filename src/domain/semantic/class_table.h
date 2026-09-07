#ifndef CYTHONPP_DOMAIN_SEMANTIC_CLASS_TABLE_H
#define CYTHONPP_DOMAIN_SEMANTIC_CLASS_TABLE_H

#include <algorithm>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "class_lookup.h"
#include "type.h"

namespace cythonpp::domain::semantic {

// The concrete ClassLookup: builtin exception/container classes seeded at
// construction, plus whatever the (not-yet-written) checking pass declares
// as it walks user `class` statements.
//
// Nested classes are keyed by their QUALIFIED name ("Outer.Inner"), the
// convention AnnotationResolver::resolve_attribute already relies on --
// is_class("Inner") is false once only "Outer.Inner" has been declared,
// matching mypy's own identity for a nested class.
//
// Three names -- EnvironmentError, IOError, WindowsError -- are not distinct
// builtin classes but the SAME class object as OSError under another
// spelling (see builtin_class_table.h's kBuiltinClassAliases). Every public
// query canonicalises its name argument first, so `Class("IOError")` behaves
// identically to `Class("OSError")` everywhere: is_class, bases_of,
// member_type, member_declared_line, constructor_type and inherits_builtin.
class ClassTable : public ClassLookup {
public:
    // Seeds itself from kBuiltinClasses (builtin_class_table.h).
    ClassTable();

    bool is_class(const std::string& name) const override;

    // Direct bases only, after canonicalising `name`. Empty for a name that
    // is not a class.
    std::vector<std::string> bases_of(const std::string& name) const override;

    // The name under which this class is actually known. An alias resolves
    // to its canonical spelling (canonical_name("IOError") == "OSError");
    // every other name -- including a canonical one -- resolves to itself.
    std::string canonical_name(const std::string& name) const;

    // Declares a class (possibly nested, via a qualified name) with its
    // direct bases. Bases are stored as given; a base that never gets its
    // own declare() call, or names a seeded builtin, is resolved lazily by
    // the transitive queries below.
    void declare(std::string qualified_name, std::vector<std::string> bases);

    void declare_member(const std::string& qualified_name, std::string member, Type type,
                         int declared_line);
    void declare_method(const std::string& qualified_name, std::string method, Type signature);

    // Depth-first, left to right through the base chain (not a real C3
    // linearisation -- mypy itself rejects the cases where that would show,
    // as a `misc` error this spec puts out of scope). Guarded against a
    // cycle in the base chain, which declare() cannot prevent.
    std::optional<Type> member_type(const std::string& qualified_name,
                                     const std::string& member) const;
    std::optional<int> member_declared_line(const std::string& qualified_name,
                                             const std::string& member) const;

    // `__init__`'s parameters minus `self`, returning Class(canonical_name).
    // A class with no declared __init__ is a nullary callable returning the
    // instance.
    Type constructor_type(const std::string& qualified_name) const;

    // Whether the base chain (excluding `object`, and excluding a seeded
    // exception class, which is an ordinary Class rather than a modelled
    // kind) reaches a name builtin_type_kind() recognises -- i.e. whether an
    // attribute miss on this class must be treated as "unmodellable" rather
    // than a genuine attr-defined error.
    bool inherits_builtin(const std::string& qualified_name) const;

private:
    struct Member {
        Type type;
        int declared_line = 0;
    };

    struct Entry {
        std::vector<std::string> bases;
        std::map<std::string, Member> members;
        std::map<std::string, Type> methods;
    };

    // Canonicalises, then looks the entry up directly (no transitive walk).
    const Entry* find_entry(const std::string& name) const;

    // The ONE private helper the transitive queries share, so the cycle
    // guard exists in exactly one place. Walks the base chain depth-first,
    // left to right, canonicalising each name before visiting it and
    // refusing to revisit a name already in `visited`. `extract` inspects
    // one entry (given its canonical name, for inherits_builtin's benefit)
    // and returns a result if this entry alone answers the query; the walk
    // stops at the first non-nullopt, matching "first base wins".
    template <typename Result, typename Extract>
    std::optional<Result> walk_chain(const std::string& name, std::vector<std::string>& visited,
                                      const Extract& extract) const {
        const std::string canonical = canonical_name(name);
        if (std::find(visited.begin(), visited.end(), canonical) != visited.end()) {
            return std::nullopt;
        }
        visited.push_back(canonical);

        const Entry* entry = find_entry(canonical);
        if (entry == nullptr) {
            return std::nullopt;
        }

        if (std::optional<Result> direct = extract(canonical, *entry)) {
            return direct;
        }
        for (const std::string& base : entry->bases) {
            if (std::optional<Result> found = walk_chain<Result>(base, visited, extract)) {
                return found;
            }
        }
        return std::nullopt;
    }

    std::map<std::string, Entry> classes_;
    std::map<std::string, std::string> aliases_;
};

} // namespace cythonpp::domain::semantic

#endif // CYTHONPP_DOMAIN_SEMANTIC_CLASS_TABLE_H
