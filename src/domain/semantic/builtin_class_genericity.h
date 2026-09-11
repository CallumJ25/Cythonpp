#ifndef CYTHONPP_DOMAIN_SEMANTIC_BUILTIN_CLASS_GENERICITY_H
#define CYTHONPP_DOMAIN_SEMANTIC_BUILTIN_CLASS_GENERICITY_H

#include <optional>
#include <string>

namespace cythonpp::domain::semantic {

// Does mypy accept a TYPE ARGUMENT on the builtin class spelled `name`?
//
//   std::nullopt -- `name` is not a class in Python's `builtins` module at
//                   all. For AnnotationResolver that means a class THIS
//                   PROGRAM declares (a bare name reaching a subscript there
//                   has already passed ClassLookup::is_class), and there are
//                   no user-defined generics in this subset.
//   true         -- generic in typeshed: `name[int]` is something mypy either
//                   accepts outright or complains about for a reason other
//                   than "expects no type arguments" (wrong arity, a type-var
//                   bound). This model cannot represent it, so the honest
//                   answer at a subscript is NotImplementedError.
//   false        -- mypy reports `"name" expects no type arguments` for it.
//                   A subscript is then a genuine error in both oracles, so
//                   `'name' is not subscriptable` is the right TypeError.
//
// WHY THIS FUNCTION EXISTS AT ALL. Its predecessor was a seven-name list
// written by hand inside annotation_resolver.cpp's anonymous namespace
// (`zip map filter enumerate reversed staticmethod classmethod`). A name IN
// that list got the correct NotImplementedError; a name MISSING from it got a
// false `TypeError: 'X' is not subscriptable` on a program `mypy --strict`
// and CPython both accept. Five names were missing -- `type`, `slice`,
// `memoryview`, `ExceptionGroup`, `BaseExceptionGroup` -- so `x: type[int]`,
// which CLAUDE.md itself devotes a paragraph to, was rejected. This is the
// exact anti-pattern builtin_class_table.h is generated to avoid, except
// that here the forgotten name is not silent: it is a false rejection.
//
// WHY IT READS A GENERATED FIELD RATHER THAN ITS OWN TABLE. The answer now
// lives in builtin_class_table.h, on the same row as the class's name and
// bases, emitted by the same generator run -- so "a builtin class whose
// genericity nobody recorded" is not a reachable state. You cannot add a row
// without answering the question. There is no second list to forget to
// update, which is the property the hand-written version could not have.
//
// WHY MYPY IS THE ORACLE FOR THE FIELD, not the running interpreter: measured
// on CPython 3.14.2, `filter[int]`, `map[int]`, `reversed[int]`, `zip[int]`
// and `slice[int]` all raise TypeError at runtime while mypy accepts every
// one, so runtime subscriptability would have DEMOTED four names the hand
// list already had right. Annotations are not evaluated (PEP 649), so CPython
// has no opinion on `x: slice[int]` at all; mypy is the only oracle with one.
// The generator's own comment carries the measurement.
std::optional<bool> builtin_class_accepts_type_arguments(const std::string& name);

} // namespace cythonpp::domain::semantic

#endif // CYTHONPP_DOMAIN_SEMANTIC_BUILTIN_CLASS_GENERICITY_H
