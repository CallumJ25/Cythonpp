#ifndef CYTHONPP_DOMAIN_SEMANTIC_NARROWING_MAP_H
#define CYTHONPP_DOMAIN_SEMANTIC_NARROWING_MAP_H

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "class_lookup.h"
#include "domain/ast/expr.h"
#include "type.h"

namespace cythonpp::domain::semantic {

// A narrowing key: a root name followed by zero or more `.attr` links,
// rendered as its dotted spelling and compared SYNTACTICALLY.
//
// Keying syntactically is not a shortcut, it is the specification. Verified
// against mypy 1.18.1: `b = self; b.n = 7` leaves `self.n` at its declared
// type and narrows only `b.n`. A "smarter" implementation that followed the
// alias would diverge from mypy, and diverging in the narrowing direction is
// how a false TypeError gets made.
//
// `self` is NOT special: an ordinary parameter `b: Bag` narrows `b.n` exactly
// as a method narrows `self.n`.
using NarrowedPath = std::string;

// One control-flow edge's worth of narrowings. A path ABSENT from the state
// means "use that path's declared type" -- there is no sentinel entry for it,
// which is why join_narrowings needs to be told the declared type.
using NarrowingState = std::map<NarrowedPath, Type>;

// `expr` as a path, or std::nullopt when it is not one.
//
// Anything containing a CALL is not a path and never narrows: a call may
// return a different object each time, so a syntactic key would be a lie
// about identity.
//
// SUBSCRIPT LINKS ARE EXCLUDED, deliberately and by scope rather than by
// oversight. All three claims below were measured against mypy 1.18.1, with
// `items: list[Inner]` and `Inner.n: object`.
//
// mypy DOES key on integer-literal subscripts index-sensitively: after
// `self.items[0].n = 7`, `self.items[0].n` reveals `builtins.int` while
// `self.items[1].n` on the next line reveals `builtins.object`. That is real
// but rare, and omitting it is safe because the fallback is the declared
// type, i.e. a missed error.
//
// TWO TRAPS FOR WHOEVER ADDS THEM. A subscript may never be the LAST link:
// with `vals: list[object]`, `self.vals[0] = 7` leaves `self.vals[0]` at
// `builtins.object`, because the read goes back through `__getitem__` and
// its declared return type wins. And a VARIABLE index must key nothing at
// all: with an `i: int` parameter, `self.items[i].n = 7` leaves
// `self.items[i].n` at `builtins.object`.
std::optional<NarrowedPath> narrowing_path_of(const ast::Expr& expr);

// The narrowing state of the code currently being checked: a map from path to
// current type, LAYERED OVER each path's declared type. A missing entry means
// "use the declared type"; the declared type is the annotation, or -- for an
// un-annotated local or an un-annotated `self.x = ...` -- the first
// assignment, which is exactly the rule the rest of this pass already
// implements.
//
// A pure data structure with no diagnostics sink of its own, the same shape as
// ScopeStack: it holds state and answers questions, and every decision about
// whether something is an error belongs to its caller.
//
// THE FEATURE IS ENTIRELY INTRAPROCEDURAL, and that is a measurement, not a
// simplification. Verified against mypy 1.18.1: a method that assigns
// `self.n = 7`, then calls another method which assigns `self.n = "reset"`,
// still reads `self.n` as `builtins.int` afterwards. Narrowing survives every
// call, including one that provably invalidates it. So there is no call graph
// here, no alias analysis and no purity analysis -- reproducing mypy is
// easier than being sound, and being sound where mypy is not would mean
// rejecting programs mypy accepts.
class NarrowingMap {
public:
    // The READ rule: this path's entry if present, else nullopt, meaning the
    // caller should use the declared type.
    std::optional<Type> get(const NarrowedPath& path) const;

    // The ASSIGN rule's install half. The caller is responsible for having
    // already checked `type` against the path's DECLARED type and for NOT
    // calling this when that check failed: the declared type is a permanent
    // ceiling, so narrowing never restricts what may be assigned next and
    // never widens it.
    void set(const NarrowedPath& path, Type type);

    // The KILL rule: remove `path`'s entry and every entry whose path has
    // `path` as a PROPER DOTTED prefix. `self.b = B()` invalidates
    // `self.b.c.n`, because that is no longer the same object.
    //
    // The dot boundary is load-bearing: `self.b` must not touch `self.bc`,
    // which is an unrelated attribute that merely shares a textual prefix.
    //
    // NOTHING ELSE INVALIDATES ANYTHING. In particular no call does -- see
    // the class comment.
    void kill(const NarrowedPath& path);

    // What resets the whole map, stated per construct rather than as one
    // exclusive claim -- four constructs, four measurements against mypy
    // 1.18.1, because "ONLY X resets" is itself a claim that needs its own
    // counterexample check before it ships.
    //
    // A REAL `def` RESETS. After `self.n = 7`, a nested `def` reading
    // `self.n` sees the DECLARED `object`, and `self.n + 1` inside it is a
    // genuine "Unsupported operand types" error.
    //
    // A NESTED `class` BODY ALSO RESETS. After `self.n = 7`, `class Local:`
    // reading `self.n` in its body also sees the DECLARED `object` --
    // `reveal_type(self.n)` inside the class body reveals `builtins.object`,
    // while a `reveal_type(self.n)` immediately before the class statement
    // and another immediately after both reveal `builtins.int`, so the reset
    // does not leak past the class body either. The concrete consequence:
    // `class Local: v = self.n + 1` draws a genuine "Unsupported operand
    // types" error from mypy --strict, while CPython runs the same file and
    // prints the narrowed `8` -- the oracles disagree, and getting this
    // wrong (treating the class body as narrowed) would make cythonpp side
    // with CPython and silently accept a program mypy rejects.
    //
    // A COMPREHENSION BODY AND AN UNANNOTATED LAMBDA BODY DO NOT RESET.
    // Narrowing crosses into both: `[self.n + 1 for _ in range(3)]` reveals
    // `builtins.list[builtins.int]`, and `h = lambda: self.n + 1` reveals
    // `def () -> builtins.int`, on a file mypy --strict accepts and CPython
    // runs (printing `8`).
    //
    // One wrinkle, recorded so it is not mistaken later for a boundary: a
    // lambda that has an EXPECTED type from its context (an annotated
    // assignment, an argument position, an element of a `list[Callable[...]]`)
    // does get its body checked against the declared, un-narrowed type, and
    // mypy reports the operand error there. The unannotated case above is the
    // one that decides this rule, because a boundary that clears would break
    // it while no boundary at all merely misses the annotated case's error.
    //
    // THE ASYMMETRY, and which way to err when unsure about a construct not
    // yet measured: failing to reset at a `def` or a `class` body reads a
    // narrowed value where mypy has already forgotten it, which SILENTLY
    // ACCEPTS a program mypy rejects -- the direction this compiler must
    // never take, because it breaks the union rule outright. Resetting at a
    // comprehension or a lambda body, on the other hand, only invents a FALSE
    // TypeError on a program both oracles already accept -- wrong, but the
    // safe wrong: a missed error, not a fabricated one. When a new construct's
    // boundary status is not yet measured, treating it as resetting (like
    // `def`/`class`) is the direction that cannot silently accept what mypy
    // would reject.
    void clear();

    // For the join: capture the state on one edge, put another edge's state
    // back. Paired with restore by an RAII guard at every call site, never by
    // hand -- a skipped restore corrupts every later read in the file.
    NarrowingState snapshot() const;
    void restore(NarrowingState state);

private:
    NarrowingState entries_;
};

// The JOIN rule, at a control-flow merge: a path's type is the union over
// reachable incoming edges, where an edge that never assigned it contributes
// its DECLARED type (supplied by `declared_type_of`, since a NarrowingState
// has no entry for it to read).
//
// A UNION, NEVER A WIDEN, and that distinction is measured: `if f:
// self.n = 7 else: self.n = "s"` reveals `builtins.int | builtins.str`, not
// `builtins.object`. So a "same on all edges, else declared" shortcut is
// wrong and this really does need a union at every merge.
//
// TWO REASONS A PATH GETS NO ENTRY, neither of which is "it was not
// narrowed":
//
//  1. `declared_type_of` cannot resolve it. Nothing resolvable means nothing
//     to layer a narrowing over, so the path is dropped rather than guessed
//     at.
//  2. The union is EQUIVALENT to the declared type, by mutual is_subtype and
//     NOT by operator==. That distinction is the whole point: == is exact and
//     order-sensitive, so it would store `bool | int` over a declared `int`
//     (mypy reveals `builtins.int`), `int | float` over a declared `float`
//     (mypy reveals `builtins.float`), and would keep or drop the same two
//     branches over a declared `int | str` depending only on which branch
//     came first. A stored Union defers every operator applied to it, so
//     storing one where the declared type already says the same thing turns
//     a working program into a NotImplementedError. `classes` is threaded in
//     for exactly this reason: `Sub | Base` over a declared `Base` reveals
//     `Base` in mypy, and recognising that needs the base chain. It may be
//     null, and two Class types are then simply unrelated.
//
//     This is also what drops an UNKNOWN join, which matters on its own:
//     Type::union_of is absorbing on Unknown, so one edge assigning from an
//     unmodellable expression would otherwise store Unknown over a perfectly
//     good declared type, and Unknown is compatible with everything in both
//     directions, so the reader would silently stop checking that path. It
//     needs no guard of its own -- is_subtype answers true whenever either
//     side is Unknown, so an Unknown value is equivalent to every declared
//     type and never gets stored.
//
// WHAT PRESENCE MEANS, AND HOW FAR THAT REACHES. In THIS FUNCTION'S OUTPUT no
// entry is even equivalent to its declared type, since (2) drops those, so
// presence here does mean "this path joined to something genuinely different
// from what it was declared as". That is a property of this function's
// output ONLY, and not of a NarrowingState in general: set() filters nothing,
// so a snapshot of a live map may well hold an entry equal to the declared
// type. Do NOT build a "was this path narrowed here?" signal on entry
// presence in a state of unknown provenance.
//
// The read rule stays the only rule either way -- an entry when there is one,
// the declared type otherwise -- and it composes, because a stored value
// equal to the declared type contributes exactly what an absent entry
// contributes to a later join.
//
// ONE NORMALISATION on top of Type::union_of: a union that contains Object
// collapses to Object, an identity rather than a heuristic, since Object is
// the top of the lattice and `T | object` IS `object`. It only ever shapes
// the value STORED, and it can be applied on either side of the equivalence
// test above without changing the answer -- unlike under ==, where the two
// orders genuinely disagreed. A caller that respects the declared type as a
// permanent ceiling never reaches it, because a union containing Object is
// then also equivalent to its declared type and dropped by (2); it is here so
// that a value which does get stored is never a Union whose Object member
// would defer every operator on it.
NarrowingState join_narrowings(
    const std::vector<NarrowingState>& edges,
    const std::function<std::optional<Type>(const NarrowedPath&)>& declared_type_of,
    const ClassLookup* classes = nullptr);

// Clears the map for the duration of a function boundary and restores it
// afterwards. RAII rather than paired calls because both users have
// report-and-return paths, and a skipped restore corrupts every later read.
class NarrowingScopeGuard {
public:
    explicit NarrowingScopeGuard(NarrowingMap& narrowings)
        : narrowings_(narrowings), saved_(narrowings.snapshot()) {
        narrowings_.clear();
    }
    ~NarrowingScopeGuard() { narrowings_.restore(std::move(saved_)); }
    NarrowingScopeGuard(const NarrowingScopeGuard&) = delete;
    NarrowingScopeGuard& operator=(const NarrowingScopeGuard&) = delete;

private:
    NarrowingMap& narrowings_;
    NarrowingState saved_;
};

// Hides INDIVIDUAL paths for the duration of a scope that is NOT a narrowing
// boundary, and puts the whole map back afterwards. The distinction from
// NarrowingScopeGuard above is the whole point: that one clears everything
// because the construct forgets everything, this one clears only the paths a
// construct REBINDS while leaving every other narrowing readable through it.
//
// The construct that needs it is a comprehension, whose own target shadows an
// enclosing name of the same spelling. A NarrowedPath carries no scope
// qualification, so an entry installed for the enclosing `x` would otherwise
// be read for the comprehension's own, entirely unrelated, `x`. Verified
// against mypy 1.18.1: with `x: object` narrowed to `int`,
// `[x + 1 for x in ["a", "b"]]` is `Unsupported operand types for +
// ("str" and "int")` -- the comprehension's `x` is a `str`, and reading the
// enclosing narrowing there silently accepts a program mypy rejects (CPython
// raises `TypeError: can only concatenate str (not "int") to str` on the
// same file, so both oracles reject it).
//
// RESTORE, NOT A PERMANENT KILL, and that too is measured: the enclosing
// narrowing survives PAST the comprehension. `[str(x) for x in ["a", "b"]]`
// followed by `reveal_type(x)` reveals `builtins.int`, so the shadowing is
// scoped to the comprehension exactly as the name binding is.
//
// Restoring the whole map is sound here only because expression typing
// installs no narrowings of its own -- every set() in this pass comes from a
// statement's assignment, and a comprehension contains no statements. If an
// expression ever starts narrowing (a walrus target would), this guard must
// become a per-path save/restore instead of a whole-map one.
class NarrowingShadowGuard {
public:
    explicit NarrowingShadowGuard(NarrowingMap& narrowings)
        : narrowings_(narrowings), saved_(narrowings.snapshot()) {}

    // Hide `path` and, via NarrowingMap::kill's own prefix rule, everything
    // beneath it -- which is what makes an ATTRIBUTE root work for free:
    // shadowing the bare target `b` also drops `b.n`, so
    // `[b.n + 1 for b in items]` reads the fresh `b`'s DECLARED `n` rather
    // than the enclosing `b.n`'s narrowing.
    void shadow(const NarrowedPath& path) { narrowings_.kill(path); }

    ~NarrowingShadowGuard() { narrowings_.restore(std::move(saved_)); }
    NarrowingShadowGuard(const NarrowingShadowGuard&) = delete;
    NarrowingShadowGuard& operator=(const NarrowingShadowGuard&) = delete;

private:
    NarrowingMap& narrowings_;
    NarrowingState saved_;
};

} // namespace cythonpp::domain::semantic

#endif // CYTHONPP_DOMAIN_SEMANTIC_NARROWING_MAP_H
