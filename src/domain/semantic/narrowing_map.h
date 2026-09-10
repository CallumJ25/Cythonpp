#ifndef CYTHONPP_DOMAIN_SEMANTIC_NARROWING_MAP_H
#define CYTHONPP_DOMAIN_SEMANTIC_NARROWING_MAP_H

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

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
// oversight. Measured: mypy DOES key on integer-literal subscripts
// index-sensitively -- `self.items[0].n` narrows and `self.items[1].n` on the
// next line does not, and a variable index narrows nothing. That is real but
// rare, and omitting it is safe because the fallback is the declared type,
// i.e. a missed error. TWO TRAPS FOR WHOEVER ADDS THEM: a subscript may never
// be the LAST link (`self.items[0] = 7` does not narrow `self.items[0]`,
// because the read goes back through `__getitem__` and its declared return
// type wins), and a variable index must key nothing at all.
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

    // Every function boundary resets the whole map. Measured: a nested `def`
    // reading `self.n` sees the DECLARED type, and using it arithmetically is
    // a genuine mypy error -- so this is the one wall where getting it wrong
    // produces a FALSE NEGATIVE rather than a false positive. Lambdas and
    // comprehension bodies are function boundaries too.
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
// ONE NORMALISATION on top of Type::union_of: a union that contains Object
// collapses to Object. That is an identity rather than a heuristic -- Object
// is the top of the lattice, so `T | object` IS `object` -- and it is what
// makes the `if`-without-`else` case come out as the measured
// `builtins.object` instead of the equivalent-but-differently-spelled
// `int | object`, whose Union kind would then defer every operator on it.
//
// A path `declared_type_of` cannot resolve is DROPPED rather than guessed at:
// nothing resolvable means nothing to layer a narrowing over. So is a joined
// type equal to the declared type, keeping "a missing entry means the
// declared type" true in both directions.
NarrowingState join_narrowings(
    const std::vector<NarrowingState>& edges,
    const std::function<std::optional<Type>(const NarrowedPath&)>& declared_type_of);

} // namespace cythonpp::domain::semantic

#endif // CYTHONPP_DOMAIN_SEMANTIC_NARROWING_MAP_H
