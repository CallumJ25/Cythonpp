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

    // The name every read query below actually looks up through, in
    // precedence order:
    //   1. a live SCOPE-LIMITED alias (declare_scoped_alias) -- these exist
    //      only while the block that declared them is being walked, and each
    //      one is by construction a SHADOWING binding, so it must outrank
    //      even an entry under the exact same spelling (a function-local
    //      `class L` shadows a module-level `class L` for the rest of that
    //      function, exactly as Python's own name binding does);
    //   2. a live entry under the EXACT spelling (so a user class declared
    //      under a builtin alias spelling, e.g. `class IOError: ...`,
    //      resolves to itself, not to OSError);
    //   3. the seeded, PERMANENT builtin alias mapping (see
    //      builtin_class_table.h's kBuiltinClassAliases) --
    //      canonical_name("IOError") == "OSError" when "IOError" is
    //      undeclared.
    // Every other name, including an already canonical one, resolves to
    // itself.
    std::string canonical_name(const std::string& name) const override;

    // A SCOPE-LIMITED alias: one extra spelling under which an
    // already-declared class is reachable, for as long as the caller keeps
    // it installed. Deliberately kept in its own map, separate from the
    // seeded builtin ones (IOError -> OSError and friends), because those
    // are PERMANENT and these are not: only these may be removed, and only
    // these outrank a live entry under the exact same spelling.
    //
    // The one caller today is TypeChecker::visit(ClassDef) for a
    // FUNCTION-LOCAL class, which is declared under a synthetic, isolated
    // qualified name (so two same-named local classes in different functions
    // cannot overwrite each other, and neither leaks to later module-level
    // code) and needs its BARE source-level name to resolve back to that
    // entry -- but ONLY inside the function that declares it.
    //
    // Returns whatever `alias` resolved to in this map BEFORE the call
    // (nullopt when it was not scope-aliased at all), so an RAII caller can
    // RESTORE it rather than delete it and shadowing composes by plain stack
    // discipline: an inner function's own `class L` may shadow an outer
    // one's, and the outer one is still reachable once the inner function's
    // body is done.
    std::optional<std::string> declare_scoped_alias(std::string alias, std::string target);
    void remove_scoped_alias(const std::string& alias);

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

    // What `name` would have canonicalised to had the SCOPE-LIMITED aliases
    // not existed -- canonical_name's steps (2) and (3) only -- but ONLY
    // when `name` is in fact scope-aliased right now AND that alias really
    // is shadowing something. nullopt otherwise, which is the common case:
    // a function-local class whose name collides with nothing shadows
    // nothing, so there is nothing to fall back to and behaviour is
    // unchanged.
    //
    // Exists for the MISS-FALLBACK below. A `Class` Type carries a name
    // STRING that is re-canonicalised at every use, so a `Class("L")`
    // resolved OUTSIDE a function that happens to declare its own
    // `class L` re-resolves, inside that function, to the LOCAL class --
    // and a member lookup on it then misses and reports a FALSE
    // attr-defined error on mypy-clean code (fix round 4's one open
    // critical). The name-based representation cannot tell the two live
    // meanings of "L" apart, so the queries below CONTAIN the damage
    // instead: on a MISS through a scoped alias they retry against the
    // shadowed class rather than concluding the attribute does not exist.
    //
    // TWO RESIDUALS this containment does NOT close, both MISSED errors
    // (never false ones), and both needing the architectural fix -- eager
    // resolution of `Class` names to keys at binding time, so a name is
    // never re-canonicalised -- rather than another patch here:
    //
    //  - is_subtype's dual. `w: L = obj` inside the shadowing function is
    //    silently ACCEPTED: both sides canonicalise to the SAME isolated
    //    key, so the mismatch between the local `L` and the module-level
    //    one is invisible. That is a spurious HIT, not a miss, so no
    //    miss-fallback can see it.
    //  - the NESTED shadow chain. shadowed_name jumps straight to the
    //    non-scoped resolution, so with `def outer: class L` /
    //    `def inner: class L`, a value typed by OUTER's local `L` used
    //    inside `inner` falls back to the MODULE-level `L` (or to nothing),
    //    never to outer's. Closing that needs scoped_aliases_ to hold a
    //    per-name STACK of targets, which would fold away the
    //    restore-not-erase contract declare_scoped_alias's return value
    //    exists to serve.
    std::optional<std::string> shadowed_name(const std::string& name) const;

    // walk_chain's body, minus the root's canonicalisation -- so a fallback
    // walk can start from an ALREADY-resolved root without canonical_name
    // routing it straight back through the very scoped alias the fallback
    // exists to bypass.
    template <typename Result, typename Extract>
    std::optional<Result> walk_resolved(const std::string& resolved,
                                        std::vector<std::string>& visited,
                                        const Extract& extract) const {
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

    // walk_chain, plus the scoped-alias MISS FALLBACK (see shadowed_name).
    // The four queries that answer "does this class have this member" share
    // this; is_class, bases_of and constructor_type deliberately DO NOT:
    //
    //  - is_class/bases_of never MISS for an isolated local class at all
    //    (declare_isolated_class always writes an entry, possibly with no
    //    bases), so a fallback there would fire on a legitimate empty
    //    answer and hand back the SHADOWED class's bases.
    //  - constructor_type must keep constructing the LOCAL class: falling
    //    back to the shadowed class's __init__ would take its PARAMETERS,
    //    so `class L: def __init__(self, a: int)` shadowed by a local
    //    `class L: pass` would make the mypy-clean `L()` a false
    //    "too few arguments". That is a NEW false positive, i.e. exactly
    //    what this round exists to remove.
    //
    // Safety of the fallback for the four that DO use it: it only ever runs
    // where the lookup had ALREADY missed, and a miss on a Class receiver is
    // already a diagnostic (or an inherits_builtin suppression). So it can
    // turn a diagnostic into a different diagnostic, or into silence, but it
    // can never turn silence into a diagnostic -- no mypy-clean program we
    // accept today can start being rejected. The accepted cost is a MISSED
    // error: a genuine attr-defined on the LOCAL class whose name the
    // shadowed class happens to define resolves instead of reporting.
    template <typename Result, typename Extract>
    std::optional<Result> query_chain(const std::string& name, const Extract& extract) const {
        std::vector<std::string> visited;
        if (std::optional<Result> found = walk_chain<Result>(name, visited, extract)) {
            return found;
        }
        const std::optional<std::string> shadowed = shadowed_name(name);
        if (!shadowed.has_value()) {
            return std::nullopt;
        }
        std::vector<std::string> shadowed_visited;
        return walk_resolved<Result>(*shadowed, shadowed_visited, extract);
    }

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
        return walk_resolved<Result>(canonical_name(name), visited, extract);
    }

    std::map<std::string, Entry> classes_;
    // The seeded, PERMANENT builtin alias mapping (IOError -> OSError, ...).
    // Never written to after construction, never removed from.
    std::map<std::string, std::string> aliases_;
    // Scope-limited aliases (declare_scoped_alias): installed and removed as
    // the checking pass enters and leaves the block that declares them, and
    // consulted BEFORE classes_ so they shadow. Kept apart from aliases_
    // precisely because that difference in lifetime and precedence must not
    // be expressible by accident.
    std::map<std::string, std::string> scoped_aliases_;
};

} // namespace cythonpp::domain::semantic

#endif // CYTHONPP_DOMAIN_SEMANTIC_CLASS_TABLE_H
