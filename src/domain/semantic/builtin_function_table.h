#ifndef CYTHONPP_DOMAIN_SEMANTIC_BUILTIN_FUNCTION_TABLE_H
#define CYTHONPP_DOMAIN_SEMANTIC_BUILTIN_FUNCTION_TABLE_H

#include <cstddef>

namespace cythonpp::domain::semantic {

// Every name in Python's `builtins` module that is callable and is NOT a
// class. GENERATED -- do not edit by hand. Regenerate with:
//
//     python scripts/verify_corpus_labels.py --generate-function-table
//
// Extracted from Python 3.14.2 on Windows.
//
// WHY THIS FILE EXISTS. `len` is invisible to this compiler for a structural
// reason: builtin_class_table.h holds CLASSES, builtin_type_names.h holds
// model KINDS, and a builtin function is neither -- so both of
// ExpressionTyper::type_of_name's carve-outs miss it and `f = len` was
// `NameError: name 'len' is not defined` on code mypy accepts. The same gap
// made every CALL to an unmodelled builtin function a false NameError too:
// `hash(x)` was `NameError: name 'hash' is not defined`.
//
// WHY EXTRACTED RATHER THAN CURATED, the same reason builtin_class_table.h
// gives: a hand-picked list has exactly one failure mode -- a forgotten name
// -- and it is silent.
//
// COMPLEMENTARY TO builtin_class_table.h BY CONSTRUCTION: that file's filter
// is `isinstance(getattr(builtins, name), type)` and this one's is
// `callable(...) and not isinstance(..., type)`, so no name appears in both
// and `int`/`str`/`list` stay classes.
//
// SITE-ADDED NAMES, recorded rather than hidden: `exit`, `quit`, `copyright`,
// `credits`, `license` and `help` are callable instances installed by the
// `site` module, not true builtins, and they appear here because the
// extraction cannot tell the difference. typeshed declares all of them in
// builtins, so treating them as defined agrees with mypy; and even if it did
// not, a SURPLUS name here is a missed error, never a false one.
//
// THIS FILE CARRIES NO SIGNATURES, deliberately. A name found here resolves
// to Unknown, which is absorbing -- so `f = len` is clean and `f([1, 2])`
// reports nothing, and `y: type = len` (which mypy rejects) becomes a missed
// error. Modelling real signatures for builtin functions is separate work;
// builtin_call_table.h is where a modelled one goes.

constexpr const char* kBuiltinFunctions[] = {
    "abs",
    "aiter",
    "all",
    "anext",
    "any",
    "ascii",
    "bin",
    "breakpoint",
    "callable",
    "chr",
    "compile",
    "copyright",
    "credits",
    "delattr",
    "dir",
    "divmod",
    "eval",
    "exec",
    "exit",
    "format",
    "getattr",
    "globals",
    "hasattr",
    "hash",
    "help",
    "hex",
    "id",
    "input",
    "isinstance",
    "issubclass",
    "iter",
    "len",
    "license",
    "locals",
    "max",
    "min",
    "next",
    "oct",
    "open",
    "ord",
    "pow",
    "print",
    "quit",
    "repr",
    "round",
    "setattr",
    "sorted",
    "sum",
    "vars",
};

constexpr std::size_t kBuiltinFunctionCount =
    sizeof(kBuiltinFunctions) / sizeof(kBuiltinFunctions[0]);

} // namespace cythonpp::domain::semantic

#endif // CYTHONPP_DOMAIN_SEMANTIC_BUILTIN_FUNCTION_TABLE_H
