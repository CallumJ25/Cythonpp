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
            // base_type_for_name performs the same classification the test
            // fake FakeClassLookup needs for its own name-keyed bases: a name
            // this model represents as a kind becomes that kind's bare Type
            // (empty args, the recorded imprecision builtin_base_type's own
            // comment documents); anything else is an ordinary Class. A
            // SOURCE-level base does not come through here at all --
            // TypeChecker::base_types resolves those through
            // AnnotationResolver, so a parametric base keeps its arguments.
            entry.bases.push_back(base_type_for_name(base));
        }

        // The constructor arity band, stored as ITS OWN field rather than a
        // synthetic entry in `methods["__init__"]` -- an earlier version of
        // this seeding did the latter, and it hijacked
        // `constructor_check`'s walk: that walk's per-entry lambda tests
        // `entry.methods.find("__init__")` FIRST, so a synthetic entry made
        // the walk return `Reached::DeclaredInit` (and hence `Checked`)
        // for ANY subclass whose base chain reaches a seeded row -- BEFORE
        // the `BuiltinKindBase -> Unmodellable` arm and BEFORE the
        // whole-chain `__new__` fallback, both of which must keep winning
        // over a seeded band. Measured: `class Singleton(object): def
        // __new__(cls, tag: int) -> "Singleton": ...` is mypy-clean and
        // CPython-clean, and the synthetic-__init__ version reported a false
        // `too many arguments for "Singleton"` by hijacking DeclaredInit via
        // `object`'s own seeded row; `class Pair(float)` / `class
        // Point(tuple[int, int])`, each with their own `__new__`, turned the
        // sanctioned `NotImplementedError` (BuiltinKindBase) into the same
        // false TypeError. Storing the band on `Entry::builtin_arity`
        // instead and consulting it ONLY at `constructor_check`'s final
        // `!reached.has_value()` fallback (see that function) restores the
        // exact positional contract: a declared `__init__`, a builtin-kind
        // base, a `BaseException` base and a declared `__new__` all still
        // win over a seeded band, anywhere in the chain.
        entry.builtin_arity = std::make_pair(builtin.min_args, builtin.max_args);
        // See Entry::is_seeded_builtin's own comment -- this is the ONE place
        // a row is ever marked seeded; declare() is the one place that mark
        // is ever removed.
        entry.is_seeded_builtin = true;
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
    // A real `class` statement SHADOWING a builtin spelling (`class slice:
    // pass`) reuses this same pre-seeded Entry -- see this constructor's own
    // comment on `bases` for why an entry is pre-seeded at all. `bases` is
    // overwritten above for exactly that reason; `builtin_arity` must be
    // reset alongside it, or the shadowing class silently inherits the
    // builtin's own arity band instead of getting an ordinary, fresh
    // zero-arg constructor. Measured both directions: `class memoryview:
    // pass` / `memoryview()` is mypy-clean and CPython-clean but reported a
    // false `too few arguments for "memoryview"` with the band left in
    // place, and `class slice: pass` / `slice(1, 2, 3)` -- both oracles
    // reject it -- went silent instead of reporting, because the
    // (unbounded) band survived and routed the shadowing class through
    // constructor_check's Unchecked fallback.
    entry.builtin_arity.reset();
    // `is_seeded_builtin` gets the identical treatment, for the identical
    // reason: a shadowing `class slice: pass` or `class Exception: pass`
    // must lose the "seeded" mark alongside the arity band and the stale
    // bases, or inherits_builtin_class would keep answering true for a class
    // this compiler can now see the real members of, and a genuine
    // attr-defined error (`class slice: pass` then `s.nope`, `class
    // Exception: pass` then a subclass then `e.nope` -- both oracles reject
    // both) would silently downgrade to NotImplementedError instead.
    entry.is_seeded_builtin = false;
    // `members`/`methods` get the same treatment, defensively: nothing in
    // ClassTable's own constructor populates either for a seeded builtin row
    // today (only `bases` and `builtin_arity`), so this is a no-op right
    // now. Leaving it unstated invites exactly the bug class `builtin_arity`
    // above exists to close, the moment a future change seeds either map for
    // a builtin row: clearing here means a shadowing class can never inherit
    // stale state through EITHER map, not just the one this fix happened to
    // add.
    //
    // THE INVARIANT THIS TRADES ON, and why the trade is safe: declare() is
    // NOT idempotent with respect to accumulated members the way it might
    // look -- declare("K") / declare_member("K", "v") / declare("K") again
    // would silently drop "v". That sequence never happens, but for two
    // DIFFERENT reasons at declare()'s two call sites, not one blanket rule:
    //
    //  - `classes_.declare(...)` inside declare_class_recursive is Phase 1's
    //    own call (from collect_classes' module-wide walk), and it declares
    //    a given qualified_name exactly once per compilation, strictly
    //    before Phase 2/3 ever call declare_member/declare_method for that
    //    same name.
    //  - `classes_.declare(...)` inside declare_isolated_class is NOT a
    //    Phase 1 call at all, despite the name -- it runs from Phase 3's
    //    visit(ClassDef), for exactly two cases: a top-level class that LOST
    //    a same-name collision, or a class lexically inside a `def`. Both
    //    isolate the class under a SYNTHETIC key embedding '#' plus the
    //    declaration's own source line (a character no Python identifier can
    //    contain), so each call's key is one Phase 1/2 never declared or
    //    populated -- there is nothing on it yet to lose -- and
    //    visit(ClassDef) walks straight into that same class's own
    //    pre_collect_class_body/check_suite immediately afterward, which is
    //    what populates it for the first and only time.
    //
    // If a future change ever calls declare() a second time for a class
    // whose members already exist -- to support re-opening one, say -- this
    // clearing must move or be conditioned on that being the class's first
    // declaration, or it will silently erase real members already collected.
    entry.members.clear();
    entry.methods.clear();
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
    } else {
        // No DECLARED __init__ anywhere in the chain. Ordinarily that means
        // a genuine zero-arg constructor (params stays empty, defaulted 0),
        // which is correct for an ordinary user class. But the chain may
        // reach a seeded builtin row carrying its own bounded arity band
        // (`Entry::builtin_arity`, set by the constructor above and reset by
        // `declare()` if a user class shadows the spelling) -- `memoryview`,
        // `property`, `staticmethod`, the bounded exception classes, and so
        // on, none of which declare a real `__init__` this model can see.
        // `find_builtin_arity` walks the WHOLE chain, not just the resolved
        // class's own entry: `class cached(property): pass` (no `__init__`
        // of its own) must still inherit `property`'s (0, 4) band, or
        // `cached(getter)` is a false "too many arguments" -- measured
        // mypy-clean and CPython-clean. Only a genuinely seeded row ever
        // carries a band at all (declare() resets it for any shadowed
        // name), so this cannot pick up a false band from an ordinary user
        // ancestor. An UNBOUNDED band (kUnboundedArity) never reaches here
        // in practice, since constructor_check routes that case to Unchecked
        // before this signature's param count is ever consulted for an
        // arity check -- but leaving it at zero-arg here regardless is
        // harmless either way.
        const std::optional<std::pair<int, int>> arity = find_builtin_arity(resolved);
        if (arity.has_value() && arity->second != kUnboundedArity) {
            const int min_args = arity->first;
            const int max_args = arity->second;
            params.assign(static_cast<std::size_t>(max_args), Type::unknown());
            defaulted = static_cast<std::size_t>(max_args - min_args);
        }
    }
    return Type::callable(std::move(params), Type::class_of(resolved), defaulted);
}

ClassTable::ConstructorCheck
ClassTable::constructor_check(const std::string& qualified_name) const {
    const std::string resolved = canonical_name(qualified_name);

    // What the ONE walk below can find, in the order the walk itself reaches
    // it -- never in a fixed priority order, which is the whole point (see
    // the header's POSITION MATTERS paragraph). Three separate whole-chain
    // walks, one per question, answered "is there an __init__ ANYWHERE" and
    // so could never see that a builtin or BaseException base to the LEFT
    // gets there first.
    enum class Reached {
        DeclaredInit,
        BaseExceptionBase,
        BuiltinKindBase,
    };

    std::vector<std::string> visited;
    const std::optional<Reached> reached = walk_chain<Reached>(
        resolved, visited,
        [](const std::string& canonical, const Entry& entry) -> std::optional<Reached> {
            // A DECLARED __init__, not one that merely resolves: an implicit
            // object.__init__ must not shadow a base further along. Matches
            // constructor_type's own declared-__init__ search, so the two
            // cannot disagree about which classes have one.
            if (entry.methods.find("__init__") != entry.methods.end()) {
                return Reached::DeclaredInit;
            }
            if (canonical == "BaseException") {
                return Reached::BaseExceptionBase;
            }
            // object is excluded for inherits_builtin's reason: every class
            // conceptually derives from it, so counting it would make every
            // constructor unmodellable. A seeded exception class is an
            // ordinary Class rather than a modelled kind, so it reaches
            // BaseException above rather than here.
            if (canonical != "object" && builtin_type_kind(canonical).has_value()) {
                return Reached::BuiltinKindBase;
            }
            return std::nullopt;
        });

    if (reached.has_value() && *reached == Reached::DeclaredInit) {
        return ConstructorCheck::Checked;
    }

    // A builtin-kind base decides Unmodellable OUTRIGHT, before the __new__
    // fallback ever runs. The __new__ walk below is whole-chain, not
    // positional, so if it ran unconditionally here it would find a __new__
    // declared ANYWHERE in the chain -- including on a base to the RIGHT of
    // the builtin base that already settled the question -- and answer
    // Unchecked (silence) for a call neither oracle accepts. Verified against
    // mypy 1.18.1 and CPython with `class Mixin: def __new__(cls, a: int) ->
    // Marker: ...` / `def __init__(self, a: str) -> None: ...` and
    // `class MyInt(int, Mixin): pass`: `MyInt(1, 2, 3)` is `No overload
    // variant of "MyInt" matches argument types "int", "int", "int"` under
    // mypy and `TypeError: int() takes at most 2 arguments (3 given)` under
    // CPython -- both oracles reject it, so this must stay Unmodellable, not
    // fall through to Mixin's unrelated __new__.
    if (reached.has_value() && *reached == Reached::BuiltinKindBase) {
        return ConstructorCheck::Unmodellable;
    }

    // The __new__ fallback, deliberately a WHOLE-CHAIN question rather than a
    // positional one (see the header): this model does not represent __new__
    // at all, so there is no signature whose position could matter. Reached
    // here only when the positional walk above found no builtin base -- i.e.
    // reached is either absent or BaseExceptionBase -- so this cannot
    // override the builtin-base answer.
    std::vector<std::string> new_visited;
    if (walk_chain<Type>(resolved, new_visited,
                         [](const std::string&, const Entry& entry) -> std::optional<Type> {
                             const auto it = entry.methods.find("__new__");
                             return it == entry.methods.end() ? std::nullopt
                                                              : std::optional<Type>(it->second);
                         })
            .has_value()) {
        return ConstructorCheck::Unchecked;
    }

    // CONSULTED LAST, DELIBERATELY, in BOTH arms below -- this is THE ENTIRE
    // POINT of storing the band on `Entry::builtin_arity` instead of in
    // `methods["__init__"]` (see the constructor's own comment): a declared
    // `__init__` anywhere in the chain, a builtin-kind base and a declared
    // `__new__` anywhere in the chain (all three checked above, unconditionally,
    // before either arm below runs at all) must always win over a seeded
    // band. Moving either lookup below any earlier in this function silently
    // changes behaviour: `complex(1)` would flip from the sanctioned
    // `NotImplementedError` (via BuiltinKindBase, since `complex` is an
    // unbounded row and would hit Unchecked first if checked ahead of it) to
    // silent acceptance.
    //
    // `find_builtin_arity` walks the WHOLE chain (not just the resolved
    // class's own entry): a subclass with no `__init__`/`__new__` of its own
    // but a bounded builtin ancestor must still inherit that ancestor's own
    // band -- `class cached(property): pass` and `class
    // MyU(UnicodeDecodeError): pass` are both mypy-clean/CPython-clean
    // (`cached`) or both-oracles-reject (`MyU(1)`), and an earlier,
    // resolved-entry-only version of this lookup left the first a false
    // "too many arguments" and the second silent. Safe to run this late,
    // same as the resolved-entry-only version it replaces: only a genuinely
    // SEEDED builtin row ever carries a band, and declare() resets it to
    // nullopt for any name a real `class` statement writes over, so no
    // ordinary user class -- Singleton, Pair, Point, MyInt, Mixin, and so on
    // -- can ever short-circuit this walk with a false band of its own; each
    // of those still exits at DeclaredInit/BuiltinKindBase/the __new__
    // fallback above, before this walk is ever consulted.
    const std::optional<std::pair<int, int>> own_arity = find_builtin_arity(resolved);
    const bool has_unbounded_arity = own_arity.has_value() && own_arity->second == kUnboundedArity;
    const bool has_bounded_arity = own_arity.has_value() && !has_unbounded_arity;

    if (!reached.has_value()) {
        // Nothing along the chain settled the question: no declared
        // __init__, no builtin-kind or BaseException base, no __new__. For
        // an ordinary user class that is the honest "Checked against a
        // zero-arg constructor" answer. But a builtin seeded with NO bounded
        // arity at all -- `slice`, `type` -- reaches exactly this point too,
        // and for it "Checked, zero-arg" is the wrong claim: mypy's own
        // verdict on its constructor is unconstrained, not zero-arg, and
        // reporting one would be a false "too many arguments" on
        // `slice(1)`/`type(1)`, both mypy-clean and both running under
        // CPython. A BOUNDED row reaching here (`memoryview`, `property`,
        // `staticmethod`, `classmethod`, `object`) needs no override: Checked
        // is already the right answer, and constructor_type separately
        // consults the same band to build the real (non-zero-arg) signature.
        if (has_unbounded_arity) {
            return ConstructorCheck::Unchecked;
        }
        return ConstructorCheck::Checked;
    }
    // Only BaseExceptionBase can still reach here: DeclaredInit returned
    // above, and BuiltinKindBase returned above too. The default for a
    // class whose chain reaches BaseException with no declared __init__ is
    // Unchecked (a genuinely variadic *args constructor, matching
    // Exception/ValueError/a user subclass of either) -- but the five
    // bounded builtin exception classes (BaseExceptionGroup, ExceptionGroup,
    // the three Unicode*Error classes) reach BaseException through their OWN
    // base chain before this walk ever asks whether the RESOLVED root itself
    // carries a real signature, and they must still be arity-checked against
    // it rather than silently accepted at any arity.
    if (has_bounded_arity) {
        return ConstructorCheck::Checked;
    }
    return ConstructorCheck::Unchecked;
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

bool ClassTable::inherits_builtin_class(const std::string& qualified_name) const {
    // Fallback included, same as inherits_builtin: nullopt here means "no
    // seeded builtin row anywhere in this chain", which is exactly the miss
    // that lets the attr-defined check fire. `true` only ever SUPPRESSES a
    // diagnostic (into NotImplementedError), so this direction cannot
    // manufacture a false one.
    const std::optional<bool> found = query_chain<bool>(
        qualified_name, [](const std::string& canonical, const Entry& entry) -> std::optional<bool> {
            // object is excluded unconditionally, exactly as inherits_builtin
            // excludes it: it IS a seeded row (bounded constructor arity
            // (0, 0)). This only matters for a chain that EXPLICITLY reaches
            // object -- `class Plain(object): pass`, or `class Mid(object):
            // pass` / `class Leaf(Mid): pass` reaching it transitively.
            // A class with NO bases at all (`class Plain: pass`) never
            // visits object's entry in the first place (there is nothing to
            // walk), so it stays a genuine attr-defined TypeError with or
            // without this line -- it is not evidence for this exclusion,
            // only for is_seeded_builtin being false on an ordinary
            // undeclared row. What this line actually prevents: every class
            // conceptually derives from object, so counting it as an
            // ancestor's answer would make every class whose chain reaches
            // it (directly or transitively) "inherit a seeded builtin" and
            // delete this whole check for that whole shape of program.
            // Measured (ClassTable.InheritsBuiltinClassExcludesObject,
            // ClassTable.InheritsBuiltinClassExcludesObjectTransitively,
            // ExpressionTyper.AMissingAttributeOnAClassWithAnExplicitObjectBaseStaysATypeError):
            // removing this line flips exactly those three, and leaves the
            // bare-Plain case (ExpressionTyper.AMissingAttributeOnAPlainClassStaysATypeError)
            // unaffected either way.
            if (canonical == "object") {
                return std::nullopt;
            }
            if (entry.is_seeded_builtin) {
                return true;
            }
            return std::nullopt;
        });
    return found.has_value() && *found;
}

} // namespace cythonpp::domain::semantic
