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
// spelling (see builtin_class_table.h's kBuiltinClassAliases). Every read
// query resolves its name argument through the shared canonical_name/
// find_entry/walk_chain path, which tries the EXACT spelling in classes_
// first and falls back to the alias mapping only on a miss. So an
// undeclared `Class("IOError")` behaves identically to `Class("OSError")`
// everywhere: is_class, bases_of, member_type, member_declared_line,
// method_type, constructor_type and inherits_builtin -- but a user class
// legitimately declared under the alias spelling (`class IOError: ...`, legal
// ordinary Python) wins over the builtin alias, since declare() writes under
// the exact spelling and the exact spelling is now tried first.
class ClassTable : public ClassLookup {
public:
    // Seeds itself from kBuiltinClasses (builtin_class_table.h).
    ClassTable();

    bool is_class(const std::string& name) const override;

    // Direct bases only, after canonicalising `name`. Empty for a name that
    // is not a class.
    std::vector<std::string> bases_of(const std::string& name) const override;

    // The name every read query below actually looks up through: a live
    // entry under the EXACT spelling wins (so a user class declared under an
    // alias spelling, e.g. `class IOError: ...`, resolves to itself, not to
    // OSError), and only when there is no live entry does this fall back to
    // the seeded alias mapping (see builtin_class_table.h's
    // kBuiltinClassAliases) -- canonical_name("IOError") == "OSError" when
    // "IOError" is undeclared. Every other name, including an already
    // canonical one, resolves to itself.
    std::string canonical_name(const std::string& name) const override;

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

    // A method's declared signature, including its `self` parameter, found
    // transitively through the base chain. Callers that need the BOUND
    // signature drop args[0] themselves -- mypy numbers arguments from the
    // first user argument and never mentions self. Kept separate from
    // member_type: attributes and methods are declared by different call
    // sites, and a later task needs to tell them apart.
    std::optional<Type> method_type(const std::string& qualified_name,
                                     const std::string& method) const;

    // `__init__`'s parameters minus `self`, returning Class(canonical_name(...))
    // -- the QUERIED class as the lookup actually resolved it, even when the
    // `__init__` used is inherited from a base (mypy prints `def (a: int) ->
    // D` for `D(B)` with no `__init__` of its own; the constructor always
    // returns the class you asked about). A user class declared under an
    // alias spelling returns Class("IOError"), not Class("OSError"); an
    // UNDECLARED alias spelling still returns Class("OSError"), matching
    // canonical_name's fallback.
    // Found transitively through the base chain, depth-first left to right,
    // same as member_type: first `__init__` wins. A class with no `__init__`
    // anywhere in the base chain is a nullary callable returning the
    // instance. Does not check is_class first: a non-class name manufactures
    // a Class(name) callable regardless, so callers must guard with is_class
    // themselves when that distinction matters.
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
    // left to right, canonicalising each name (see canonical_name) before
    // visiting it and refusing to revisit a name already in `visited`. `extract`
    // inspects one entry (given the name the lookup resolved it to, for
    // inherits_builtin's and constructor_type's benefit) and returns a
    // result if this entry alone answers the query; the walk stops at the
    // first non-nullopt, matching "first base wins".
    template <typename Result, typename Extract>
    std::optional<Result> walk_chain(const std::string& name, std::vector<std::string>& visited,
                                      const Extract& extract) const {
        const std::string resolved = canonical_name(name);
        if (std::find(visited.begin(), visited.end(), resolved) != visited.end()) {
            return std::nullopt;
        }
        visited.push_back(resolved);

        const auto it = classes_.find(resolved);
        if (it == classes_.end()) {
            return std::nullopt;
        }
        const Entry& entry = it->second;

        if (std::optional<Result> direct = extract(resolved, entry)) {
            return direct;
        }
        for (const std::string& base : entry.bases) {
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
