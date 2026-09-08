#ifndef CYTHONPP_DOMAIN_SEMANTIC_BUILTIN_CALL_TABLE_H
#define CYTHONPP_DOMAIN_SEMANTIC_BUILTIN_CALL_TABLE_H

#include <optional>
#include <string>
#include <vector>

#include "type.h"

namespace cythonpp::domain::semantic {

// Whether `name` is a builtin call this compiler MODELS -- the set
// builtin_call_result below can answer for. Deliberately smaller than
// is_builtin_callable_name: `enumerate`, `zip`, `map`, `filter` and
// `reversed` are real, mypy-defined callables this model cannot yet type --
// each needs a generic Iterator class this project's Type has no
// constructor for -- so they are EXCLUDED here but still true under
// is_builtin_callable_name below. The pair is what lets
// ExpressionTyper::type_of_call tell "not supported" (NotImplementedError,
// since the program may be one mypy accepts) from "not defined" (NameError,
// a genuine error) apart for a Name callee.
bool is_supported_builtin_call(const std::string& name);

// Every name Python's builtins module defines that is CALLABLE, whether or
// not this model can type the call -- a SUPERSET of is_supported_builtin_call
// (it additionally contains enumerate/zip/map/filter/reversed). A callee name
// outside this set is a genuine NameError; a name inside it but outside
// is_supported_builtin_call is NotImplementedError, never NameError -- the
// name IS defined and mypy accepts the call, so NameError would be a false
// positive against the hard invariant.
bool is_builtin_callable_name(const std::string& name);

// True for the five container constructors -- list, dict, set, frozenset,
// tuple -- called with ZERO arguments, which follow the exact same
// context-or-silent-Unknown rule as an empty `[]` / `{}` display (Task 13):
// WITH a usable `expected` context, take it; WITHOUT one, say nothing and
// let the caller return Unknown silently -- the var-annotated diagnostic
// belongs to a later task's Assign arm, the only place the variable's name
// exists to put in it.
//
// Exposed separately from builtin_call_result's nullopt so the caller does
// not need its own copy of this five-name list to stay in sync with: EVERY
// other builtin's nullopt from builtin_call_result is a genuine mismatch
// (wrong arity or argument kind) and IS reported as TypeError, but nullopt
// from one of these five names called with no arguments must not be.
bool is_empty_display_builtin(const std::string& name);

// Every signature verified with real mypy 1.18.1's reveal_type -- see the
// task brief for the full table. `args` are the call's own positional
// argument types, already typed by the caller; this function performs no
// recursion into the AST and reports nothing itself, matching every other
// rule table in domain/semantic (operator_rules.h, subscript_result, ...).
//
// `expected` is the bidirectional-checking context, consulted ONLY by the
// five zero-argument container constructors above -- every other row
// ignores it.
//
// nullopt means "this exact (name, args) shape has no modelled answer",
// which the CALLER must interpret using is_empty_display_builtin above: for
// one of those five names called with zero arguments and no usable
// `expected`, nullopt means silent Unknown; for everything else, nullopt is
// a genuine TypeError.
//
// Unknown is absorbing throughout, exactly like operator_rules.h's tables:
// an Unknown argument (the residue of an already-reported failure, e.g. an
// unbound name) never by itself turns a call's own result into a fresh
// nullopt -- it is treated as satisfying whatever arity/kind check would
// otherwise apply, so one root cause still draws one diagnostic.
std::optional<Type> builtin_call_result(const std::string& name, const std::vector<Type>& args,
                                        const Type& expected);

} // namespace cythonpp::domain::semantic

#endif // CYTHONPP_DOMAIN_SEMANTIC_BUILTIN_CALL_TABLE_H
