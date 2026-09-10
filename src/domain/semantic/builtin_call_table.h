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

// Whether `name` is a builtin FUNCTION -- a name in Python's builtins module
// that is callable and is not a class (see builtin_function_table.h, which is
// generated). Complementary to builtin_class_table.h by construction; no name
// is in both.
//
// Separate from is_supported_builtin_call and is_builtin_callable_name below,
// which are about whether a CALL can be typed. This one answers the prior
// question: does this name exist at all? A resolution miss on one of these
// used to fall straight into a false NameError.
bool is_builtin_function_name(const std::string& name);

// Every builtin FUNCTION name this predicate covers, a CALL to which this
// compiler will not report as a NameError: the modelled set above, plus the
// generated builtin-function table, plus five real generic callables
// (enumerate/zip/map/filter/reversed) this model cannot type because each
// needs a generic Iterator its Type has no constructor for.
//
// NOT every non-NameError callable name -- deliberately narrower. A builtin
// CLASS used as a constructor (`object()`, `ValueError()`, `super()`,
// `memoryview()`, ...) is also never a NameError, but it is resolved through
// ClassTable before this predicate is ever consulted (see type_of_name_call),
// not through this table, so it is correctly false here (measured: `o =
// object()` is mypy-clean and draws no diagnostic from this compiler either).
// This predicate only needs to answer for a Name callee that ClassTable does
// not already know how to construct.
//
// This used to be a hand-curated list of 32 names described as "every
// callable in builtins", which it was not -- it omitted most of them (`hash`,
// `getattr`, ...) and included class names. (`sorted` was NOT one of the
// omissions -- it is the 21st entry of kSupportedBuiltinCalls below and was
// always modelled.) Consulting the generated table closes the real gap: a
// call to any builtin function is now either modelled or a named
// NotImplementedError, never a NameError, since the name IS defined and
// mypy accepts the call.
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
