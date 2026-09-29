#ifndef CYTHONPP_DOMAIN_SEMANTIC_SCOPE_STACK_H
#define CYTHONPP_DOMAIN_SEMANTIC_SCOPE_STACK_H

#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "order_position.h"
#include "type.h"

namespace cythonpp::domain::semantic {

enum class ScopeKind { Module, Class, Function, Comprehension };

// A single name binding: the type it was inferred or declared to have, WHERE
// it sits in execution order, WHERE it was first declared, and whether it
// came from an explicit annotation.
//
// THE POSITION AND THE DECLARATION LINE ARE TWO FIELDS, AND WERE ONE `int`
// (`declared_line`) UNTIL 2026-09-23. They are split because that one field
// was answering three different questions, and they were measured pulling in
// three directions:
//
//   P  position    bound_at >= statement_line_     is this name bound yet at
//                  (ExpressionTyper::type_of_name) this read?
//   S  state       bound_at == this statement      is this an unfilled
//                  (is_unfilled_placeholder)       placeholder I should fill?
//   V  provenance  "already defined on line N"     where was it FIRST bound?
//
// The dead-arm fix (`3b83611`) needed P to move PAST a statically-dead arm's
// inferred binding, so the LIVE assignment is the one that fills the
// placeholder. Moving the single field made the live binding satisfy S as
// well -- and two of the six is_unfilled_placeholder callers read a TRUE
// answer as "there is no prior definition, so do not report [no-redef]".
// FOURTEEN measured shapes went from a correct `name "x" already defined on
// line N` at exit 1 to exit 0, on programs `mypy --strict` REJECTS and
// CPython accepts: the never-acceptable direction. Found by adversarial
// review, not by the suite.
//
// The fix at the time was a `kind` whitelist (`line_may_move`) inside
// pre_bind_function_body, arbitrating between the three jobs from outside.
// Neutered 2026-09-23 to confirm it was load-bearing rather than assumed:
// dropping it failed exactly one test out of 1741,
// ALiveAnnotatedOrNestedDefBindingDoesNotTakeTheMovedLine.
struct Binding {
    // JOB S, and the reason construction goes through a NAMED factory rather
    // than aggregate initialization: whether a binding is a still-unfilled
    // PLACEHOLDER (put there by pre_bind_assignment_targets /
    // pre_bind_function_body so a read above the real assignment resolves to
    // something) or a REAL declaration used to be inferred from
    // `bound_at == the statement's line`. That is how jobs S and P came to
    // share a field, and it is what broke when P had to move (see this
    // struct's own header comment).
    //
    // A DEFAULTED `bool placeholder = false` would have been the easy version
    // and is wrong here for a reason this codebase records twice over: under
    // aggregate initialization an omitted member is value-initialized, so a
    // new defaulted field leaves all NINETEEN construction sites silently
    // meaning what they meant before, and the two that create placeholders
    // have to REMEMBER to opt in. That is exactly the "a default is the thing
    // a caller forgets" failure DiagnosticSink::report's mandatory
    // Suppressibility and CodegenMode's deliberately-absent default exist to
    // prevent. An enum with no default initializer does not help either --
    // value-initialization picks enumerator 0, so whichever state is spelled
    // first becomes the silent one.
    //
    // So: no default constructor, no aggregate init, and every site says
    // which kind it is building.
    static Binding placeholder(OrderPosition bound_at, int declared_at);
    static Binding declared(Type type, OrderPosition bound_at, int declared_at,
                            bool annotated = false, bool order_exempt = false,
                            bool method_self = false);

    Binding() = delete;

    bool is_placeholder() const { return placeholder_; }

    // THE FILL TRANSITION. Called ON the placeholder, returns the DECLARED
    // binding that replaces it -- and keeps BOTH of this placeholder's
    // positions rather than taking the filling statement's.
    //
    // Structural rather than remembered, because remembering is what fails.
    // Today `bound_at` and `declared_at` are equal everywhere, and the
    // filling statement is BY CONSTRUCTION the one whose line they hold (that
    // is what fills_placeholder tests), so stamping the filler's own line
    // would be invisible. The moment bound_at moves past a dead arm the two
    // diverge, and a fill that re-stamped declared_at would make every LATER
    // `already defined on line N` name the live line where mypy names the
    // dead arm's. Pinned by its own neutering; no pre-existing test covers it.
    Binding fill(Type filled_type, bool annotated = false, bool order_exempt = false) const;

    Type type;

    // JOB P. Where this binding sits in its scope's execution order. MAY be
    // moved past a statically-dead arm's inferred binding, which is the
    // whole reason it is no longer the same field as declared_at. See
    // OrderPosition for why it is a distinct type and not a second `int`.
    OrderPosition bound_at;

    // JOB V. The line this name was FIRST bound at in this scope, and the
    // line every `already defined on line N` diagnostic names. NEVER moves:
    // it is what a reader is told to go and look at, so it must keep
    // matching mypy's own answer. Measured -- with `if False: x = "s"` /
    // `x: int = 1` / `x = 2`, mypy says line 3, the DEAD arm's own line, so
    // this staying at the first binding overall is what reproduces mypy
    // exactly and what makes a forward line reference unrepresentable.
    int declared_at = 0;

    // True when the binding came from an explicit annotation. Re-annotating
    // an annotated name is a redefinition error; re-ASSIGNING it is an
    // ordinary assignment check.
    bool annotated = false;

    // General rule: a binding is order-exempt when the name is bound BEFORE
    // the code that may read it, so a same-line read of it can never be a
    // genuine use-before-definition. The ordinary ordering check's `>=`
    // (bound_at >= statement_line_, deliberately `>=` so `x = x + 1`
    // still trips) cannot tell "same line because bound first" apart from
    // "same line because read first" on its own -- this flag is how the
    // binding site, which does know which one it is, tells
    // ExpressionTyper::type_of_name to skip the check entirely.
    //
    // This bug class has shipped three separate times, each caught only
    // after the previous fix had already gone out, because every site looks
    // like an isolated special case until the next one turns up with the
    // same shape:
    //   1. A function parameter -- `def f(x: int) -> None: print(x)` reads
    //      `x` on the `def`'s own line; there is no separate body line for
    //      it to be strictly greater than.
    //   2. A `for` target -- `for i in range(3): print(i)`, same shape.
    //   3. A comprehension target -- `[v * v for v in values]`, same shape
    //      again. This one was found by the labelled corpus AFTER 1013 unit
    //      tests passed, and it fires on essentially every real list
    //      comprehension.
    //
    // The flag also lets assign_to/assign_name tell a parameter apart from
    // pre_bind_function_body's own "still-unfilled placeholder" pattern
    // (same test, bound_at == the current statement's line) -- without
    // it, `def f(x: int) -> None: x = "s"` would be mistaken for the
    // placeholder-fill case and silently REBIND over the parameter's
    // annotation instead of reporting the incompatible assignment.
    //
    // Before adding a fourth site: this is true exactly when the binding is
    // established before the expression(s) that could read it on the same
    // line are typed (a target bound ahead of its RHS/element/body). Leave
    // it false for anything where the read happens first or independently
    // of the bind -- a plain `x = value` RHS, a placeholder rebind, or a
    // signature bound in an outer scope that the body only resolves
    // outward into -- since those are genuine before/after cases where a
    // same-line collision would be a real use-before-definition, not a
    // same-line coincidence.
    bool order_exempt = false;

    // True ONLY for a method's own first parameter -- the `self` a `def`
    // directly inside a class body binds at index 0. False for every other
    // binding, including a parameter merely SPELLED "self" on a plain
    // function or on a nested def, and including one annotated with the
    // enclosing class.
    //
    // Exists because `self.x = ...` DECLARES an instance attribute exactly
    // when the `self` it stores through is a method's own first parameter,
    // and that question cannot be answered from the binding's TYPE (the only
    // thing TypeChecker::self_attribute_receiver_type used to consult) nor
    // from the innermost function's own method-ness. Measured 2026-09-11
    // against mypy 1.18.1 and CPython 3.14.2, both halves:
    //
    //   - TYPE alone is too LOOSE. A nested `def inner(self: Bag)` inside a
    //     Bag method binds `self` to exactly Class("Bag"), so a type-only
    //     guard passed and `self.q = 1` there declared "q" on Bag -- a later
    //     `self.q` read from another method then came out clean where mypy
    //     reports `"Bag" has no attribute "q"` at BOTH the store and the
    //     read. A missed error.
    //   - "the IMMEDIATELY ENCLOSING function is a method" is too TIGHT, and
    //     wrong in the unsafe direction. mypy attributes a store to the
    //     method's self no matter how many nested function scopes the
    //     reference is closed over: a `def inner()` inside a Bag method that
    //     writes the CAPTURED `self.q = 1` is `Success`, and so is the same
    //     store two closures deep, in both cases with a later `self.q` read
    //     from a different method also clean. Requiring the innermost
    //     function to be a method reported a false TypeError on all of those.
    //
    // What the BINDING gets right and neither of those does: the flag
    // travels with the name, so resolving outward through any number of
    // closures still finds the method's own `self` and still declares, while
    // a nested def's own shadowing parameter is a different binding and does
    // not. Set at the one parameter-binding site in
    // TypeChecker::visit(FunctionDef) from the `is_method` it already
    // computes; nothing else in this codebase binds a method parameter, and
    // no path rebinds `self` (it is order_exempt, so the placeholder-fill
    // rebind cannot mistake it for a placeholder).
    bool method_self = false;

    // The parameter NAMES of `type`, in order, when this binding was made
    // from a `def` statement -- and EMPTY whenever they are not known.
    //
    // Why they live here and not in `Type`: mypy's redefinition rule compares
    // parameter names (measured 2026-09-11, mypy 1.18.1 -- `if c: def g(a:
    // int) -> int` against a prior `def g(b: int) -> int` is `All conditional
    // function variants must have identical signatures  [misc]`, where the
    // same pair with matching names is accepted), but `Type::callable`
    // deliberately carries only {parameter types, return type, defaulted
    // count}. Putting names into `Type` was rejected: `Type` is a copied
    // value compared with an exact `operator==` by unrelated callers, its
    // Callables are also built by builtin_call_table and by
    // ClassTable::constructor_type from places that have no names to supply,
    // and two sites erase `args[0]` to bind `self` -- every one of those
    // would have to learn to keep a parallel name vector in step, and the
    // failure mode of forgetting is a signature whose names and types
    // disagree. A `Binding` is instead exactly as scoped as the question:
    // the rule asks about the binding a redefinition collides with, and
    // `ScopeStack::resolve` already hands that binding over.
    //
    // EMPTY means "not recorded", and the reader must treat it that way
    // rather than as "a zero-parameter signature": only the two `def`-binding
    // sites in TypeChecker fill it, so a binding made by an assignment of a
    // function value (`g = h`) carries h's Callable type with no names at
    // all. TypeChecker::has_identical_signature tells the two apart by
    // LENGTH against the Callable's own parameter count, and falls back to
    // comparing types only when they disagree -- which is the direction that
    // misses an error rather than inventing one. Measured: `g = h` (h taking
    // one parameter) followed by a conditional `def g` with a DIFFERENT
    // parameter name is a mypy error this compiler does not report, while the
    // same shape with a MATCHING name is mypy-clean, so "unknown names means
    // report" would have been a false positive on the second.
    //
    // Filled by assignment at the two sites that have names, never through a
    // factory. STALE RATIONALE CORRECTED 2026-09-24: this used to explain the
    // field's position in terms of "every existing brace-initialisation of a
    // Binding passes one to five members positionally", and warned that
    // `method_self` had to be inserted ABOVE it to stay inside the positional
    // prefix. Binding is no longer brace-initialisable anywhere (see the
    // factories at the top of this struct), so there is no positional prefix
    // to protect and member order is now free.
    std::vector<std::string> param_names;

    // The source-line where the OUTERMOST enclosing `for`/`while` loop this
    // binding's PLACEHOLDER (pre_bind_function_body) was created inside
    // begins, or 0 if it was not created inside a loop at all. 0 is a safe
    // sentinel: every real source line is 1-based (domain/lexer's own
    // convention). OUTERMOST, not innermost, deliberately: a read at an
    // OUTER loop's own nesting level can be checking a name a further-NESTED
    // inner loop assigns, and the two still share that OUTER loop's own
    // back-edge (measured: `for x in xs: (if started: print(total)); for y
    // in ys: total = y; started = True` -- read at the outer level, write
    // inside the inner `for` -- is mypy `Success`, CPython exit 0; tagging
    // with the INNER loop's start line alone left this one a false
    // positive).
    //
    // EXISTS TO FIX A REGRESSION: widening pre_bind_function_body to recurse
    // into If/While/For bodies (closing a genuine union-rule violation, see
    // CLAUDE.md) made the ordering check fire across a LOOP BACK-EDGE, where
    // it must not -- a loop body is not a straight line, so a read
    // TEXTUALLY ABOVE a same-scope binding inside the SAME loop body can
    // still execute AFTER it, on a later iteration. Measured against mypy
    // 1.18.1 and CPython 3.14, the canonical shape:
    //
    //   total = 0
    //   def run(xs: list[int]) -> None:
    //       started = False
    //       for x in xs:
    //           if started:
    //               print(total)
    //           total = x
    //           started = True
    //
    // is mypy `Success` and CPython prints `1`, `2` at exit 0 -- both oracles
    // accept a program the widened pre-bind pass alone would reject. mypy's
    // own `used-before-def` check is flow-sensitive here (a loop-head JOIN),
    // not line-based, and does not fire for ANY read/write pair confined to
    // one loop body regardless of a guard, of the read being unconditional, or
    // of the read and write sharing one line (`total = total + x`) --
    // measured all three, all `Success`. This field is the line-number-only
    // approximation of that join: ExpressionTyper::type_of_name treats a read
    // as exempt from the ordering check when its own line is `>=` this one.
    // No upper bound is stored or checked -- a binding's own `bound_at`
    // is always `<= ` the true end of whatever loop `loop_start_line` names,
    // by construction, and this arm is only ever reached when `bound_at
    // >= ` the read's position already, so the read is trapped below
    // `bound_at` too; storing a redundant upper bound would be untestable
    // dead data.
    //
    // GATED, in the READER (not here), on the name ALSO resolving in an
    // ENCLOSING scope (ScopeStack::bound_in_an_enclosing_scope) -- this field
    // alone is not sufficient (see the guard's own comment for why):
    // without an outer binding, mypy itself reports `Cannot determine type of
    // "x"  [has-type]` for the identical loop shape, a real rejection this
    // compiler must not silently paper over just because the write happens to
    // sit in a loop.
    //
    // Deliberately NOT preserved once a placeholder is filled in (assign_name/
    // visit(For)'s tuple-target block build a fresh Binding with the field
    // unset): by the time the real per-statement walk reaches the statement
    // that fills the placeholder, every read on an EARLIER line has already
    // been typed against this exact placeholder in the same single
    // top-to-bottom pass check_suite performs, so nothing downstream ever
    // needs to consult this field on the filled-in Binding.
    OrderPosition loop_start_line;

    // True while this binding is a mypy PARTIAL NONE type: created by an
    // UNANNOTATED assignment whose value is `None`, and not yet resolved.
    //
    // mypy does NOT treat `x = None` as declaring `x` to be `None`. It
    // records a partial type and takes the declared type from the next
    // assignment that RESOLVES it -- and the resolved type is `T | None`,
    // NOT `T`. Measured 2026-09-16 against mypy 1.18.1 and CPython 3.14, in
    // four independent forms, because getting this wrong is the difference
    // between closing a false positive and creating a missed error:
    //   - mypy's own error text names it: `x = None` / `x = 1` / `x = "s"`
    //     is `Incompatible types in assignment (expression has type "str",
    //     variable has type "int | None")`.
    //   - A later `x = None` is ACCEPTED (`x = None` / `x = 1` / `x = None`
    //     is `Success`), so the declared type genuinely absorbed None.
    //   - A cross-scope read, where mypy's binder cannot narrow, reveals
    //     `builtins.int | None`.
    //   - It is byte-identically equivalent to writing
    //     `x: int | None = None`, diffed over a whole probe file.
    //
    // THE TRAP: `reveal_type` shows the NARROWED type, not the declared one.
    // `x = None` / `x = 1` / `reveal_type(x)` is `builtins.int`, because the
    // binder knows the last write was an int. An implementation built on
    // that reads the rule as resolving to `T`, and then `def f() -> int:
    // return x` (a module-level partial read from a def BELOW the resolver)
    // flips from mypy's `got "int | None", expected "int"` to SILENTLY
    // CLEAN -- a missed error manufactured by the fix. The narrowing map is
    // what makes an in-scope `return x` right after `x = 1` still clean, so
    // both readings look identical until that cross-scope probe separates
    // them.
    //
    // NOT set for an order_exempt binding (a parameter, a `for` target, a
    // comprehension target): mypy's partial types come from ASSIGNMENTS, and
    // a parameter is annotated in this compiler's subset anyway. A `for`
    // target may still RESOLVE a partial (measured: `x = None` /
    // `for x in [1, 2]:` resolves to `int | None`), which is why resolution
    // below is not gated on the assignment's own kind.
    //
    // DELIBERATELY NOT extended to an ATTRIBUTE path (`self.x = None`), and
    // that is a measured boundary rather than a shortcut: mypy's partial for
    // `self.x` is confined to the METHOD FRAME that created it, so
    // `self.x = None` in `__init__` resolved by `self.x = 1` in ANOTHER
    // method is a real mypy error -- `Incompatible types in assignment
    // (expression has type "int", variable has type "None")`, which is the
    // identical message and line this compiler already reports. That is the
    // idiomatic shape, and it already AGREES; widening this flag to
    // attribute paths without modelling the frame rule would silently accept
    // it. See CLAUDE.md for the same-method sibling, which stays a known
    // false positive.
    bool partial_none = false;

    // The CONTAINER SHAPE of a live mypy PARTIAL CONTAINER type --
    // `list[Unknown]` or `dict[Unknown, Unknown]` -- and nullopt when this
    // binding is not one. Created by an UNANNOTATED assignment whose value is
    // a bare empty `list`/`dict` (`[]`, `{}`, `list()`, `dict()`), and only
    // when TypeChecker's own per-scope scan has already PROVEN the partial is
    // resolved before it is ever read; cleared the moment a resolver arrives.
    //
    // A SEPARATE RULE FROM partial_none, NOT AN EXTENSION OF IT, and the two
    // must not be merged. Measured 2026-09-16 against mypy 1.18.1 and CPython
    // 3.14.2, the divergence that makes them different rules:
    //   - `x = None` / `x = 1` resolves to `int | None` -- a UNION, because
    //     the declared type absorbs None (a later `x = None` is accepted).
    //   - `x = []` / `x = [1]` resolves to `list[int]` PLAINLY. There is no
    //     None in play to absorb, and a later `x = None` is a real error:
    //     `Incompatible types in assignment (expression has type "None",
    //     variable has type "list[int]")`, and that is the ONLY error mypy
    //     reports for the program, because `x = [1]` already resolved the
    //     partial. A future round reading partial_none's comment and
    //     generalising "a partial resolves to `T | None`" to containers would
    //     invent a `list[int] | None` declared type and silently accept that
    //     program. TypeChecker.AResolvedContainerPartialDoesNotAbsorbNone
    //     exists to catch exactly that.
    //
    // An optional-of-Type rather than a bool because the resolver has to know
    // WHICH container kind it is resolving: a dict display does not resolve a
    // list partial (measured -- mypy reports both `Need type annotation` and
    // an incompatible assignment for `x = []` / `x = {1: 2}`), and whether a
    // subscript STORE resolves is kind-dependent too (`dict[k] = v` does,
    // `list[0] = v` does not, measured both ways).
    //
    // The two flags can never both be live on one binding: partial_none needs
    // a value of type None and this one needs a bare empty container, whose
    // value ExpressionTyper types as Unknown -- mutually exclusive at the one
    // creation site in TypeChecker::assign_name.
    //
    // Declared last, and only ever filled by assignment after construction --
    // like param_names, and unlike everything the factories take. STALE
    // RATIONALE CORRECTED 2026-09-24: this used to justify the position by
    // appending keeping "every existing Binding{...} meaning what it says".
    // There is no aggregate initialization of Binding any more (see the
    // factories at the top of this struct), so field ORDER no longer carries
    // meaning for callers at all; only the factory parameter lists do.
    std::optional<Type> partial_container;

private:
    Binding(Type type, OrderPosition bound_at, int declared_at, bool annotated,
            bool order_exempt, bool method_self, bool placeholder)
        : type(std::move(type)), bound_at(bound_at), declared_at(declared_at),
          annotated(annotated), order_exempt(order_exempt), method_self(method_self),
          placeholder_(placeholder) {}

    bool placeholder_ = false;
};

// What a lookup found, and WHERE, because the ordering rule (3b) depends on
// whether the binding lives in the reader's own scope. ScopeStack does not
// decide whether a use-before-definition is an error -- it only reports
// where the binding was found and at what line, and a later task compares.
struct Resolution {
    const Binding* binding = nullptr;   // null when not found
    bool in_own_scope = false;
};

// A pure data structure modelling Python's lexical scoping for name
// resolution. Resolution searches the current scope, then every enclosing
// Function/Comprehension scope, then the Module scope, SKIPPING every
// enclosing Class scope on the way -- whatever kind the current scope is.
//
// The skip is a property of the scope being READ, not of the reader: a class
// body's names are visible only to the code lexically in that same body.
// That covers the familiar case (a method body cannot see its class body's
// names) and the less familiar one it used to get wrong (a class nested in a
// class cannot see the OUTER class body's names either). The current scope is
// resolved before the outward walk begins, so a class body still sees its own
// names.
class ScopeStack {
public:
    ScopeStack();                       // pushes the Module scope

    void push(ScopeKind kind);

    // Pops the current scope. A no-op when only the Module scope remains --
    // the module scope is permanent and popping it is never valid, but this
    // is a pure data structure with no diagnostics sink, so silently
    // refusing rather than asserting is the caller-safe default.
    void pop();

    ScopeKind current_kind() const;

    // Binds in the CURRENT scope. Returns false if the name is already bound
    // there, so the caller can report a redefinition.
    bool bind(const std::string& name, Binding binding);

    // Overwrites an existing binding in the current scope, for the
    // collect-then-check two-pass: the collect pass binds a signature and the
    // check pass must not report a redefinition against itself.
    void rebind(const std::string& name, Binding binding);

    // Resolution order: current scope, then enclosing Function and
    // Comprehension scopes SKIPPING every Class scope, then Module.
    Resolution resolve(const std::string& name) const;

    // True only for the current scope, for the redefinition check.
    bool bound_in_current_scope(const std::string& name) const;

    // True when `name` resolves to something in an ENCLOSING scope --
    // deliberately IGNORING the current scope's own binding, unlike
    // `resolve()`, which returns the current scope's entry the moment it
    // finds one and never even looks further out. Runs the identical
    // outward walk `resolve()` falls back to (skipping every Class scope on
    // the way), just unconditionally rather than only on a current-scope
    // miss.
    //
    // The one caller (ExpressionTyper's loop-back-edge exemption, see
    // Binding::loop_start_line) needs exactly this: a placeholder the CURRENT
    // scope already holds is not evidence either way about whether an OUTER
    // scope also binds the name, and that question decides whether mypy
    // itself has anything to say about a loop-scoped read/write pair at all
    // (`Cannot determine type of "x"  [has-type]` when there is no outer
    // binding, silence when there is) -- so the answer must come from a
    // fresh, current-scope-blind walk, not from anything `resolve()` already
    // returned.
    bool bound_in_an_enclosing_scope(const std::string& name) const;

private:
    struct Scope {
        ScopeKind kind;
        std::map<std::string, Binding> bindings;
    };

    std::vector<Scope> scopes_;
};

} // namespace cythonpp::domain::semantic

#endif // CYTHONPP_DOMAIN_SEMANTIC_SCOPE_STACK_H
