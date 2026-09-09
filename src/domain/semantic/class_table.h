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
    std::vector<Type> bases_of(const std::string& name) const override;

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
    void declare(std::string qualified_name, std::vector<Type> bases);

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

    // "Did declare_member write this member on THIS class, rather than on
    // something in its base chain?" -- the one question member_type cannot
    // answer, since it walks the chain and reports a hit either way.
    //
    // Exists because mypy's rule for RE-DECLARING an attribute is two
    // different rules depending on the answer (verified against mypy 1.18.1),
    // and that is true at all THREE re-declaration sites in type_checker.cpp
    // -- visit(AnnAssign)'s self.x branch, visit(AnnAssign)'s class-body arm,
    // and assign_to's class-body plain-Assign arm. What the two rules ARE is
    // NOT uniform across the three, so each site states and justifies its own
    // pair; only the INHERITED half is shared:
    //
    //  - INHERITED, at every site: the re-declaration installs a real,
    //    narrower per-class type, and is an error unless it is a subtype of
    //    the inherited one.
    //  - SAME CLASS: site-dependent. A METHOD-level annotation under an
    //    existing declaration is IGNORED (the earlier declaration stays the
    //    attribute's type), while a CLASS-BODY statement is itself the
    //    declaration and WINS, with the earlier declaration checked against
    //    it -- because a class-body declaration outranks a method-level one
    //    whatever the textual order.
    //
    // Conflating own with inherited costs a false TypeError in one direction
    // or the other, whichever way round that site's comparison is written.
    //
    // Deliberately does NOT canonicalise through canonical_name and does NOT
    // fall back to a shadowed class the way member_type's query_chain does:
    // it must resolve `qualified_name` to exactly the key declare_member
    // itself writes under, or it would answer a question about a DIFFERENT
    // class's member map than the one the caller is about to declare into.
    std::optional<Type> own_member_type(const std::string& qualified_name,
                                        const std::string& member) const;

    // own_member_type's line half, with own_member_type's exact lookup
    // policy: no canonicalisation, no chain walk, no scoped-alias fallback.
    //
    // Exists so a caller that gates on own_member_type and then asks "was
    // that entry written by the statement I am looking at right now?" gets
    // both answers out of the SAME member map. Pairing own_member_type with
    // the canonicalising, chain-walking member_declared_line instead reads
    // the line off a potentially DIFFERENT class's entry than the type came
    // from, so the two can disagree about which statement declared what.
    std::optional<int> own_member_declared_line(const std::string& qualified_name,
                                                const std::string& member) const;

    // The third member question, distinct from both of the two above: "what
    // does this class INHERIT for this member, ignoring whatever its own
    // entry says?" member_type cannot answer it (the class's own entry wins
    // first) and own_member_type cannot either (it never looks past that
    // entry).
    //
    // It is a question worth asking because a re-declaration's rule depends
    // on whether the type it is overriding is the class's OWN or a BASE's
    // (see own_member_type above), and by the time type_checker.cpp reaches
    // a re-declaration statement, that class's own entry already exists --
    // it was installed eagerly, before any body was checked, precisely so a
    // reader method written ABOVE the declaration sees the declared type.
    // So "is there anything of mine yet?" no longer separates the two cases,
    // and the inherited chain has to be queried directly.
    //
    // Lookup policy, deliberately split between the root and the bases: the
    // ROOT is resolved exactly as own_member_type resolves it, since it is
    // the same key its caller is about to declare_member into; each BASE is
    // resolved as member_type resolves it, since a base is a source-level
    // spelling that may be an alias. The root -- under both spellings -- is
    // seeded into the cycle guard, so a cyclic base chain (`class A(B)` /
    // `class B(A)`, or a function-local `class L(L)`) cannot walk back into
    // the queried class's own entry and return the very value this query
    // exists to bypass.
    std::optional<Type> inherited_member_type(const std::string& qualified_name,
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

    // "How should a call to this class's constructor be checked?" -- a
    // CAPABILITY answer, not a signature, and deliberately separate from
    // constructor_type for that reason.
    //
    // THREE-VALUED, and an enum rather than a pair of bool predicates, for
    // the reason RuleResult is three-valued rather than an optional and
    // OutputMode is an enum rather than a second bool: the three answers are
    // mutually exclusive, so two independent predicates would admit a
    // meaningless "both true" state and force every call site to hard-code
    // its own precedence between them. One value, one decision.
    //
    // The distinction that matters most here is between the two ways this
    // model can fail to represent a constructor, which the two arms below
    // answer DIFFERENTLY:
    //
    //  - Unchecked, for a constructor that is genuinely VARIADIC. Verified
    //    against mypy 1.18.1: reveal_type of `class MyError(Exception): pass`
    //    is `def (*args: builtins.object) -> MyError`, so MyError(),
    //    MyError("boom") and MyError("boom", 42) are ALL clean -- via that
    //    one `*args: object`, not via a dedicated overload -- and so are
    //    ValueError("bad", 1, 2) and OSError(2, "no such file"). There is no
    //    arity such a constructor rejects, so silence is not a guess: it is
    //    the right answer, and there is no diagnostic to defer. This model
    //    has no variadic Callable and inventing one for this alone is not
    //    warranted, so the honest thing is to say "do not check" rather than
    //    to hand back a signature that is a lie in one direction or the
    //    other.
    //  - Unmodellable, for a constructor that is a BOUNDED OVERLOAD SET this
    //    model cannot spell -- a base chain reaching a builtin KIND (int,
    //    str, list, ...). Measured against mypy 1.18.1 and CPython:
    //    `class MyInt(int): pass` makes MyInt(3), MyInt("ff", 16) and
    //    `class C(str): pass` C("abc") all `Success` / a clean run, but
    //    MyInt(1, 2, 3) is `No overload variant of "MyInt" matches argument
    //    types "int", "int", "int"` and dies under CPython with
    //    `TypeError: int() takes at most 2 arguments (3 given)`. Both oracles
    //    reject it, so going SILENT here would be silent acceptance of a
    //    program neither oracle accepts. Not knowing which arity is legal is
    //    exactly the "cannot model the construct" case, so the caller reports
    //    NotImplementedError -- the same answer builtin_call_table already
    //    gives for the identical gap one level down, where `int("ff", 16)`
    //    is `calls to builtin 'int' with these argument types are not
    //    supported`.
    //
    // Checked otherwise, including for every class whose constructor this
    // model CAN spell: constructor_type's signature is checked as usual.
    //
    // POSITION MATTERS, because Python and mypy resolve the constructor by
    // MRO, not by "anything in the chain wins". A single depth-first,
    // left-to-right walk decides all three answers at once, stopping at the
    // FIRST base that settles the question -- a declared __init__, a builtin
    // kind, or BaseException -- so a leftward builtin or exception base beats
    // a rightward plain one and vice versa. Verified against mypy 1.18.1,
    // with `class Mixin: def __init__(self, a: str)`:
    // `class MyInt(int, Mixin)` then MyInt(3) is `Success` (mypy takes
    // int.__new__, the earlier MRO entry) and CPython prints 3, while
    // `class MyInt(Mixin, int)` then MyInt(3) is
    // `Argument 1 to "MyInt" has incompatible type "int"; expected "str"`.
    // Three separate whole-chain walks -- one per question -- could not tell
    // those two apart, and reported the second answer for both.
    //
    // The __new__ arm is a deliberate FALLBACK, not a model of __new__, and
    // is deliberately NOT position-aware: it asks the whole chain. Measured
    // against mypy 1.18.1: __new__ participates fully when no __init__ exists
    // (a class declaring only `__new__(cls, a: int)` reveals as
    // `def (a: builtins.int) -> N`) and loses outright to __init__ when both
    // exist (`def (b: builtins.str) -> M`), with no complaint about the
    // contradiction. Rather than model that, a class with __new__ and no
    // __init__ is Unchecked, so the failure mode is a MISSED error instead of
    // a false "too few arguments" on every construction of it.
    //
    // BUT a builtin-kind base decides Unmodellable BEFORE the __new__
    // fallback ever runs, precisely because the fallback is whole-chain: a
    // __new__ declared on some OTHER base -- one that never settles the
    // question the positional walk already answered -- must not turn a
    // bounded-overload-set call into silence. Verified against mypy 1.18.1
    // and CPython with `class Mixin: def __new__(cls, a: int) -> Marker: ...`
    // plus `def __init__(self, a: str) -> None: ...`, and
    // `class MyInt(int, Mixin): pass`: `MyInt(1, 2, 3)` is `No overload
    // variant of "MyInt" matches argument types "int", "int", "int"` under
    // mypy and `TypeError: int() takes at most 2 arguments (3 given)` under
    // CPython -- both oracles reject it, so the builtin-base answer
    // (Unmodellable) must win over Mixin's unrelated __new__, which the
    // whole-chain fallback would otherwise reach.
    //
    // A DECLARED __init__ reached first always wins: an exception or
    // builtin-based subclass that defines its own constructor is checked
    // against it exactly like any other class.
    enum class ConstructorCheck {
        // constructor_type's signature is the constructor; check it.
        Checked,
        // Genuinely variadic; there is nothing to check and nothing to defer.
        Unchecked,
        // A bounded overload set this model cannot spell; defer to the caller,
        // which reports NotImplementedError.
        Unmodellable,
    };
    ConstructorCheck constructor_check(const std::string& qualified_name) const;

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
        std::vector<Type> bases;
        std::map<std::string, Member> members;
        std::map<std::string, Type> methods;
    };

    // Canonicalises, then looks the entry up directly (no transitive walk).
    const Entry* find_entry(const std::string& name) const;

    // The ClassTable KEY a base Type is looked up under during a chain walk:
    // a Class base's own name, or the builtin spelling for a base that is a
    // builtin kind (so a base recorded as list[int] still reaches `list`'s
    // seeded entry and its `object` base). std::nullopt for a base that
    // denotes no entry at all -- Unknown (a base whose annotation failed to
    // resolve, already reported), Union, Callable -- which simply ends that
    // branch of the walk.
    static std::optional<std::string> base_key(const Type& base);

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
    // attr-defined error on mypy-clean code (the one open
    // critical). The name-based representation cannot tell the two live
    // meanings of "L" apart, so the queries below CONTAIN the damage
    // instead: on a MISS through a scoped alias they retry against the
    // shadowed class rather than concluding the attribute does not exist.
    //
    // THREE RESIDUALS this containment does NOT close, all MISSED errors
    // (never false ones), and all three needing the architectural fix --
    // eager resolution of `Class` names to keys at binding time, so a name
    // is never re-canonicalised -- rather than another patch here:
    //
    //  - the BARE-spelling receiver. `obj = L()` bound OUTSIDE the function
    //    to the MODULE-level `L` still carries the bare spelling "L", and
    //    canonical_name's scoped-alias check runs first regardless of where
    //    the value came from -- so inside the function, `obj.c()` resolves
    //    straight to the LOCAL `class L` and, if it defines `c`, HITS on
    //    the wrong class instead of missing at all. No miss-fallback runs
    //    (there is no miss to fall back from), so a `c` the module-level
    //    `L` lacks but the local one defines is silently ACCEPTED.
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
        for (const Type& base : entry.bases) {
            const std::optional<std::string> key = base_key(base);
            if (!key.has_value()) {
                // A base that denotes no ClassTable entry -- Unknown from a
                // failed annotation, or a Union/Callable a base can never
                // really be. Ends this branch rather than aborting the walk.
                continue;
            }
            if (std::optional<Result> found = walk_chain<Result>(*key, visited, extract)) {
                return found;
            }
        }
        return std::nullopt;
    }

    // walk_chain, plus the scoped-alias MISS FALLBACK (see shadowed_name),
    // layered in HERE rather than inside walk_chain itself, for two
    // reasons. First, walk_chain is invoked recursively per base inside the
    // chain walk, so a fallback placed there would fire once per base name
    // during every walk, not once; query_chain fires exactly once, at the
    // root. Second, an explicit opt-in list of which queries get the
    // fallback beats an implicit exclusion that would hold only by
    // accident -- see constructor_type below, whose safety from this
    // fallback today comes entirely from a pre-canonicalisation step at its
    // own call site, not from anything walk_chain or query_chain does; if a
    // future change passed constructor_type a bare, unresolved name
    // instead, an implicit exclusion would silently become a false "too
    // few arguments", while an explicit list simply would not include it.
    //
    // Applied to the four queries that answer "does this class have this
    // member": member_type, member_declared_line, method_type and
    // inherits_builtin. is_class, bases_of and constructor_type are
    // deliberately excluded, each for a different reason:
    //
    //  - is_class cannot miss in the first place while the alias points at
    //    a live isolated entry (declare_isolated_class always writes one,
    //    possibly with no bases), so there is no miss for a fallback to
    //    catch.
    //  - bases_of's empty result for that same isolated entry is a
    //    LEGITIMATE answer, not a miss -- it feeds is_subtype's chain walk,
    //    where substituting the shadowed class's bases would manufacture
    //    spurious subtype hits rather than merely miss an attribute.
    //  - constructor_type would take the shadowed class's __init__
    //    PARAMETERS: `class L: def __init__(self, a: int)` shadowed by a
    //    local `class L: pass` would make the mypy-clean `L()` a false
    //    "too few arguments" -- exactly what the fallback exists to remove.
    //
    // Safety of the fallback for the four that DO use it: it only ever runs
    // where the lookup had ALREADY missed, and a miss on a Class receiver is
    // already a diagnostic (or an inherits_builtin suppression). So it can
    // turn a diagnostic into a different diagnostic, or into silence, but it
    // can never turn silence into a diagnostic -- no mypy-clean program we
    // accept today can start being rejected. The accepted cost is exactly
    // the three residuals shadowed_name documents above (the bare-spelling
    // receiver, is_subtype's dual, and the nested shadow chain) -- all
    // MISSED errors, never false ones.
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
