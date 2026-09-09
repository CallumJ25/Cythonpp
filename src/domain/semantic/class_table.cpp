#include "domain/semantic/class_table.h"

#include <algorithm>
#include <cstddef>
#include <utility>

#include "domain/semantic/builtin_class_table.h"
#include "domain/semantic/builtin_type_names.h"
#include "domain/semantic/type_compatibility.h"

namespace cythonpp::domain::semantic {

ClassTable::ClassTable() {
    // Aliases first: seeding below needs to know which names are aliases so
    // it can skip them, not give them a dead entry of their own.
    for (std::size_t index = 0; index < kBuiltinClassAliasCount; ++index) {
        const BuiltinClassAlias& alias = kBuiltinClassAliases[index];
        aliases_.emplace(alias.alias, alias.canonical);
    }
    for (std::size_t index = 0; index < kBuiltinClassCount; ++index) {
        const BuiltinClass& builtin = kBuiltinClasses[index];
        // An alias name (IOError, EnvironmentError, WindowsError) is the SAME
        // class object as its canonical (OSError), not a distinct one, so it
        // gets no entry of its own -- every query canonicalises before
        // touching classes_, so an entry here would only ever be reached by
        // a direct, uncanonicalised write (declare/declare_member/
        // declare_method), and leaving it pre-seeded with a duplicate of the
        // canonical's bases meant such a write silently clobbered that
        // duplicate instead of creating a fresh entry.
        if (aliases_.find(builtin.name) != aliases_.end()) {
            continue;
        }
        Entry& entry = classes_[builtin.name];
        for (const char* base : builtin.bases) {
            if (base == nullptr) {
                continue;
            }
            // A seeded base is a bare NAME, so it carries no type arguments.
            // A name this model represents as a kind becomes that kind's bare
            // Type (empty args, the recorded imprecision builtin_base_type
            // already documents); anything else is an ordinary Class.
            const std::optional<TypeKind> kind = builtin_type_kind(base);
            entry.bases.push_back(kind.has_value() ? builtin_base_type(*kind)
                                                   : Type::class_of(base));
        }
    }
}

std::optional<std::string> ClassTable::base_key(const Type& base) {
    if (base.kind == TypeKind::Class) {
        return base.name;
    }
    return builtin_type_spelling(base.kind);
}

std::string ClassTable::canonical_name(const std::string& name) const {
    // A live SCOPE-LIMITED alias wins over everything, including an entry
    // under the identical spelling: one of these only exists while the block
    // that declared it is being walked, and it is by construction a
    // SHADOWING binding (a function-local `class L` shadows a module-level
    // `class L` for the rest of that function, exactly as Python's own name
    // binding does). Resolving to the module-level entry instead would point
    // a member lookup at the WRONG class, which is a false attr-defined
    // error waiting to happen.
    const auto scoped = scoped_aliases_.find(name);
    if (scoped != scoped_aliases_.end()) {
        return scoped->second;
    }
    // Then a live entry under the exact spelling -- this is what makes a
    // user class declared under a builtin alias spelling (`class IOError:
    // ...`, legal ordinary Python) reachable, since declare() writes under
    // the exact spelling with no canonicalisation. Only on a miss does the
    // builtin alias mapping apply, and only on a miss there does the name
    // resolve to itself.
    if (classes_.find(name) != classes_.end()) {
        return name;
    }
    const auto it = aliases_.find(name);
    return it == aliases_.end() ? name : it->second;
}

std::optional<std::string> ClassTable::declare_scoped_alias(std::string alias,
                                                            std::string target) {
    std::optional<std::string> previous;
    const auto it = scoped_aliases_.find(alias);
    if (it != scoped_aliases_.end()) {
        previous = it->second;
        it->second = std::move(target);
        return previous;
    }
    scoped_aliases_.emplace(std::move(alias), std::move(target));
    return previous;
}

void ClassTable::remove_scoped_alias(const std::string& alias) { scoped_aliases_.erase(alias); }

std::optional<std::string> ClassTable::shadowed_name(const std::string& name) const {
    if (scoped_aliases_.find(name) == scoped_aliases_.end()) {
        // Not scope-aliased, so canonical_name did not re-route this lookup
        // and there is nothing for a fallback to mean.
        return std::nullopt;
    }
    // Deliberately NOT the previous SCOPED target (declare_scoped_alias's own
    // return value): what a function-local `class L` shadows is almost always
    // the module-level entry under the exact spelling -- canonical_name's step
    // (2) -- and there was no previous scoped alias at all in that case.
    if (classes_.find(name) != classes_.end()) {
        return name;
    }
    const auto it = aliases_.find(name);
    if (it != aliases_.end()) {
        return it->second;
    }
    // Scope-aliased but shadowing NOTHING: the ordinary function-local class
    // whose name collides with no other class. Unchanged behaviour.
    return std::nullopt;
}

const ClassTable::Entry* ClassTable::find_entry(const std::string& name) const {
    const auto it = classes_.find(canonical_name(name));
    return it == classes_.end() ? nullptr : &it->second;
}

bool ClassTable::is_class(const std::string& name) const { return find_entry(name) != nullptr; }

std::vector<Type> ClassTable::bases_of(const std::string& name) const {
    const Entry* entry = find_entry(name);
    return entry == nullptr ? std::vector<Type>{} : entry->bases;
}

void ClassTable::declare(std::string qualified_name, std::vector<Type> bases) {
    Entry& entry = classes_[std::move(qualified_name)];
    entry.bases = std::move(bases);
}

void ClassTable::declare_member(const std::string& qualified_name, std::string member, Type type,
                                int declared_line) {
    const auto it = classes_.find(qualified_name);
    if (it == classes_.end()) {
        // No declare() call ever named this class: a mis-spelled or
        // mis-ordered call by the checking pass must not fabricate one --
        // that would silently turn a NameError into a clean annotation.
        return;
    }
    it->second.members[std::move(member)] = Member{std::move(type), declared_line};
}

void ClassTable::declare_method(const std::string& qualified_name, std::string method,
                                Type signature) {
    const auto it = classes_.find(qualified_name);
    if (it == classes_.end()) {
        return;
    }
    it->second.methods[std::move(method)] = std::move(signature);
}

std::optional<Type> ClassTable::member_type(const std::string& qualified_name,
                                            const std::string& member) const {
    return query_chain<Type>(
        qualified_name, [&member](const std::string&, const Entry& entry) -> std::optional<Type> {
            const auto it = entry.members.find(member);
            if (it == entry.members.end()) {
                return std::nullopt;
            }
            return it->second.type;
        });
}

std::optional<Type> ClassTable::own_member_type(const std::string& qualified_name,
                                                const std::string& member) const {
    // declare_member's own lookup, verbatim -- no canonicalisation, no chain
    // walk, no scoped-alias fallback (see the header for why each is wrong
    // here).
    const auto entry = classes_.find(qualified_name);
    if (entry == classes_.end()) {
        return std::nullopt;
    }
    const auto it = entry->second.members.find(member);
    if (it == entry->second.members.end()) {
        return std::nullopt;
    }
    return it->second.type;
}

std::optional<int> ClassTable::own_member_declared_line(const std::string& qualified_name,
                                                        const std::string& member) const {
    // own_member_type's lookup, verbatim -- see the header for why the two
    // must share one policy.
    const auto entry = classes_.find(qualified_name);
    if (entry == classes_.end()) {
        return std::nullopt;
    }
    const auto it = entry->second.members.find(member);
    if (it == entry->second.members.end()) {
        return std::nullopt;
    }
    return it->second.declared_line;
}

std::optional<Type> ClassTable::inherited_member_type(const std::string& qualified_name,
                                                      const std::string& member) const {
    const auto entry = classes_.find(qualified_name);
    if (entry == classes_.end()) {
        return std::nullopt;
    }
    // Both spellings of the root are seeded into the cycle guard before the
    // walk starts: the exact key (which is what declare_member wrote under)
    // and whatever it canonicalises to (which is what a base naming this
    // same class would resolve to). Without that seed a cyclic base chain
    // walks straight back into the root's own members and returns the entry
    // this query exists to look PAST.
    std::vector<std::string> visited{qualified_name};
    const std::string canonical_root = canonical_name(qualified_name);
    if (canonical_root != qualified_name) {
        visited.push_back(canonical_root);
    }
    const auto extract = [&member](const std::string&,
                                   const Entry& base_entry) -> std::optional<Type> {
        const auto it = base_entry.members.find(member);
        if (it == base_entry.members.end()) {
            return std::nullopt;
        }
        return it->second.type;
    };
    for (const Type& base : entry->second.bases) {
        const std::optional<std::string> key = base_key(base);
        if (!key.has_value()) {
            // A base that denotes no ClassTable entry (Unknown, Union,
            // Callable) -- see base_key's own comment. Ends this branch.
            continue;
        }
        if (std::optional<Type> found = walk_chain<Type>(*key, visited, extract)) {
            return found;
        }
        // The scoped-alias MISS FALLBACK query_chain applies at its own root,
        // applied here once per base -- a base is exactly the kind of
        // source-level spelling that fallback exists for. The cycle guard is
        // shared with the walk above rather than restarted, so the fallback
        // cannot re-enter the root either.
        const std::optional<std::string> shadowed = shadowed_name(*key);
        if (shadowed.has_value()) {
            if (std::optional<Type> found = walk_resolved<Type>(*shadowed, visited, extract)) {
                return found;
            }
        }
    }
    return std::nullopt;
}

std::optional<int> ClassTable::member_declared_line(const std::string& qualified_name,
                                                    const std::string& member) const {
    // Shares member_type's fallback deliberately: a caller gates on this
    // having a value and then dereferences member_type (type_checker.cpp's
    // class-body placeholder disambiguation), so the two must miss and hit
    // in lockstep or that dereference is on an empty optional.
    return query_chain<int>(
        qualified_name, [&member](const std::string&, const Entry& entry) -> std::optional<int> {
            const auto it = entry.members.find(member);
            if (it == entry.members.end()) {
                return std::nullopt;
            }
            return it->second.declared_line;
        });
}

std::optional<Type> ClassTable::method_type(const std::string& qualified_name,
                                            const std::string& method) const {
    return query_chain<Type>(
        qualified_name, [&method](const std::string&, const Entry& entry) -> std::optional<Type> {
            const auto it = entry.methods.find(method);
            if (it == entry.methods.end()) {
                return std::nullopt;
            }
            return it->second;
        });
}

Type ClassTable::constructor_type(const std::string& qualified_name) const {
    // canonical_name resolves through the same exact-spelling-wins precedence
    // every read query shares: a user class declared under an alias spelling
    // (e.g. `class IOError: ...`) resolves to itself, not to OSError. Only an
    // UNDECLARED alias spelling falls back to the builtin's canonical name.
    const std::string resolved = canonical_name(qualified_name);

    // Transitive, through the shared walk_chain, so the cycle guard is
    // inherited rather than re-implemented: an inherited __init__ IS the
    // subclass's constructor signature (class D(B): pass with B declaring
    // __init__ means D(...) takes B's parameters), and a naive non-guarded
    // transitive search over a base-chain cycle would hang.
    std::vector<std::string> visited;
    const std::optional<Type> init = walk_chain<Type>(
        resolved, visited,
        [](const std::string&, const Entry& entry) -> std::optional<Type> {
            const auto it = entry.methods.find("__init__");
            return it == entry.methods.end() ? std::nullopt : std::optional<Type>(it->second);
        });

    std::vector<Type> params;
    // Carried over from __init__'s own signature: `self` never has a default,
    // so stripping it changes the parameter COUNT but not how many of the
    // trailing ones are optional. Without this, `class G:` with
    // `def __init__(self, name: str = "world")` would make `G()` a false
    // "too few arguments" -- the constructor is the one signature in this
    // file that is rebuilt rather than handed back as stored, so it is also
    // the one place the count could be silently lost.
    std::size_t defaulted = 0;
    if (init.has_value()) {
        // init->args is [self, param..., return], return last. Strip both
        // ends: self is not a caller-supplied argument, and the return is
        // replaced below by the instance type of the QUERIED class, not the
        // declaring one -- mypy prints `def (a: int) -> D` for D(B) even
        // though B declared __init__.
        for (std::size_t i = 1; i + 1 < init->args.size(); ++i) {
            params.push_back(init->args[i]);
        }
        defaulted = std::min(init->defaulted_params, params.size());
    }
    return Type::callable(std::move(params), Type::class_of(resolved), defaulted);
}

bool ClassTable::inherits_builtin(const std::string& qualified_name) const {
    // Fallback included: nullopt here means "no builtin in this chain", which
    // is exactly the miss that lets the attr-defined check fire. Effectively
    // an OR across the local class and the one it shadows, and `true` only
    // ever SUPPRESSES a diagnostic, so this direction cannot manufacture one.
    const std::optional<bool> found = query_chain<bool>(
        qualified_name, [](const std::string& canonical, const Entry&) -> std::optional<bool> {
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
