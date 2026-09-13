#ifndef CYTHONPP_DOMAIN_SEMANTIC_BUILTIN_CLASS_TABLE_H
#define CYTHONPP_DOMAIN_SEMANTIC_BUILTIN_CLASS_TABLE_H

#include <cstddef>

namespace cythonpp::domain::semantic {

// max_args == kUnboundedArity means "no arity constraint this model
// records". Measured 2026-09-12: every row this generator classifies as
// unbounded also accepts ZERO arguments under the probe below, so unbounded
// always means unconstrained -- there is no row needing "min 3, max
// unbounded".
constexpr int kUnboundedArity = -1;

// Every name in Python's `builtins` module that is a class, with its direct
// bases. GENERATED -- do not edit by hand. Regenerate with:
//
//     python scripts/verify_corpus_labels.py --generate-class-table
//
// Extracted from Python 3.14.2 on Windows.
//
// WHY EXTRACTED RATHER THAN CURATED. `x: type`, `x: Exception`,
// `x: BaseException`, `x: slice` and `x: memoryview` are all mypy --strict
// clean and all drew `NameError: name 'type' is not defined`. A hand-picked
// list has exactly one failure mode -- a forgotten name -- and it is silent.
//
// WHY BASES ARE INCLUDED. Seeding the names base-less would close one
// invariant-(a) hole and open another: `e: Exception = ValueError()` is
// mypy-clean, and with no bases the Class-to-Class subtype check fails and we
// emit a false TypeError.
//
// PLATFORM SENSITIVITY, recorded rather than hidden: `WindowsError` appears on
// Windows (it is OSError) and is absent on Linux. A missing name is a
// direction-(b) gap; a surplus name is a missed error. Neither breaks the hard
// invariant.
//
// `object` is deliberately NOT recorded as anyone's base: the model spells the
// top type as TypeKind::Object, not as a class, and is_subtype already returns
// true for any source against target Object.
//
// Names that ARE model kinds (int, str, list, ...) appear here too and are
// harmless: builtin_type_kind() is consulted before ClassLookup::is_class().
//
// WHY `accepts_type_arguments` IS GENERATED TOO, and why the interpreter is
// NOT the oracle for it. `x: zip[int]` is mypy-CLEAN (zip is generic in
// typeshed), so AnnotationResolver must answer a subscripted generic builtin
// with NotImplementedError; answering "'zip' is not subscriptable" would be a
// TypeError on a program both oracles accept. That routing used to consult a
// SEVEN-NAME HAND-WRITTEN list in annotation_resolver.cpp, and the list was
// missing `type`, `slice`, `memoryview`, `ExceptionGroup` and
// `BaseExceptionGroup` -- five measured false positives, `x: type[int] = int`
// among them. So the flag is extracted, for exactly the reason the names and
// bases are.
//
// The extraction asks MYPY, not the running interpreter, because the
// interpreter gives the WRONG answer. Measured on Python 3.14.2:
// `filter[int]`, `map[int]`, `reversed[int]`, `zip[int]` and `slice[int]` all
// raise TypeError at runtime (no __class_getitem__) while mypy accepts every
// one of them, and `type[int]` is accepted by both. Runtime subscriptability
// would therefore have DEMOTED four names the old hand list already had
// right. The question the union rule cares about is the static one -- does
// mypy accept a type argument here -- so the generator writes one
// `def f(a: NAME[int]) -> None` line per class name, runs `mypy --strict`
// over it in a temporary directory, and records False for exactly those names
// mypy answers `"NAME" expects no type arguments` for. Every other verdict
// (clean, a different arity, a type-var bound complaint) means generic, and
// an unrecognised verdict makes the generator raise rather than guess.

// Four is enough: the widest direct-base list in the extraction is 2. The
// generator raises if that ever stops being true.
struct BuiltinClass {
    const char* name;
    const char* bases[4];

    // False only for a class mypy reports `"X" expects no type arguments`
    // for. See the paragraph above for why this is mypy-derived rather than
    // interpreter-derived, and builtin_class_genericity.h for the one
    // consumer.
    bool accepts_type_arguments;

    // The constructor arity band mypy accepts, derived by probing
    // `NAME(a(), a(), ...)` with `def a() -> Any: ...` so argument TYPES
    // cannot mask an arity verdict, and filtering to the `[call-arg]` code.
    // An overload mismatch is `[call-overload]` and deliberately does NOT
    // count, so an overloaded class (`type`, which accepts exactly {1, 3})
    // reads as unbounded -- a missed error, the safe direction, never a
    // false positive.
    int min_args;
    int max_args;
};

constexpr BuiltinClass kBuiltinClasses[] = {
    {"ArithmeticError", {"Exception", nullptr, nullptr, nullptr}, false, 0, -1},
    {"AssertionError", {"Exception", nullptr, nullptr, nullptr}, false, 0, -1},
    {"AttributeError", {"Exception", nullptr, nullptr, nullptr}, false, 0, -1},
    {"BaseException", {nullptr, nullptr, nullptr, nullptr}, false, 0, -1},
    {"BaseExceptionGroup", {"BaseException", nullptr, nullptr, nullptr}, true, 2, 2},
    {"BlockingIOError", {"OSError", nullptr, nullptr, nullptr}, false, 0, -1},
    {"BrokenPipeError", {"ConnectionError", nullptr, nullptr, nullptr}, false, 0, -1},
    {"BufferError", {"Exception", nullptr, nullptr, nullptr}, false, 0, -1},
    {"BytesWarning", {"Warning", nullptr, nullptr, nullptr}, false, 0, -1},
    {"ChildProcessError", {"OSError", nullptr, nullptr, nullptr}, false, 0, -1},
    {"ConnectionAbortedError", {"ConnectionError", nullptr, nullptr, nullptr}, false, 0, -1},
    {"ConnectionError", {"OSError", nullptr, nullptr, nullptr}, false, 0, -1},
    {"ConnectionRefusedError", {"ConnectionError", nullptr, nullptr, nullptr}, false, 0, -1},
    {"ConnectionResetError", {"ConnectionError", nullptr, nullptr, nullptr}, false, 0, -1},
    {"DeprecationWarning", {"Warning", nullptr, nullptr, nullptr}, false, 0, -1},
    {"EOFError", {"Exception", nullptr, nullptr, nullptr}, false, 0, -1},
    {"EncodingWarning", {"Warning", nullptr, nullptr, nullptr}, false, 0, -1},
    {"EnvironmentError", {"Exception", nullptr, nullptr, nullptr}, false, 0, -1},
    {"Exception", {"BaseException", nullptr, nullptr, nullptr}, false, 0, -1},
    {"ExceptionGroup", {"BaseExceptionGroup", "Exception", nullptr, nullptr}, true, 2, 2},
    {"FileExistsError", {"OSError", nullptr, nullptr, nullptr}, false, 0, -1},
    {"FileNotFoundError", {"OSError", nullptr, nullptr, nullptr}, false, 0, -1},
    {"FloatingPointError", {"ArithmeticError", nullptr, nullptr, nullptr}, false, 0, -1},
    {"FutureWarning", {"Warning", nullptr, nullptr, nullptr}, false, 0, -1},
    {"GeneratorExit", {"BaseException", nullptr, nullptr, nullptr}, false, 0, -1},
    {"IOError", {"Exception", nullptr, nullptr, nullptr}, false, 0, -1},
    {"ImportError", {"Exception", nullptr, nullptr, nullptr}, false, 0, -1},
    {"ImportWarning", {"Warning", nullptr, nullptr, nullptr}, false, 0, -1},
    {"IndentationError", {"SyntaxError", nullptr, nullptr, nullptr}, false, 0, -1},
    {"IndexError", {"LookupError", nullptr, nullptr, nullptr}, false, 0, -1},
    {"InterruptedError", {"OSError", nullptr, nullptr, nullptr}, false, 0, -1},
    {"IsADirectoryError", {"OSError", nullptr, nullptr, nullptr}, false, 0, -1},
    {"KeyError", {"LookupError", nullptr, nullptr, nullptr}, false, 0, -1},
    {"KeyboardInterrupt", {"BaseException", nullptr, nullptr, nullptr}, false, 0, -1},
    {"LookupError", {"Exception", nullptr, nullptr, nullptr}, false, 0, -1},
    {"MemoryError", {"Exception", nullptr, nullptr, nullptr}, false, 0, -1},
    {"ModuleNotFoundError", {"ImportError", nullptr, nullptr, nullptr}, false, 0, -1},
    {"NameError", {"Exception", nullptr, nullptr, nullptr}, false, 0, -1},
    {"NotADirectoryError", {"OSError", nullptr, nullptr, nullptr}, false, 0, -1},
    {"NotImplementedError", {"RuntimeError", nullptr, nullptr, nullptr}, false, 0, -1},
    {"OSError", {"Exception", nullptr, nullptr, nullptr}, false, 0, -1},
    {"OverflowError", {"ArithmeticError", nullptr, nullptr, nullptr}, false, 0, -1},
    {"PendingDeprecationWarning", {"Warning", nullptr, nullptr, nullptr}, false, 0, -1},
    {"PermissionError", {"OSError", nullptr, nullptr, nullptr}, false, 0, -1},
    {"ProcessLookupError", {"OSError", nullptr, nullptr, nullptr}, false, 0, -1},
    {"PythonFinalizationError", {"RuntimeError", nullptr, nullptr, nullptr}, false, 0, -1},
    {"RecursionError", {"RuntimeError", nullptr, nullptr, nullptr}, false, 0, -1},
    {"ReferenceError", {"Exception", nullptr, nullptr, nullptr}, false, 0, -1},
    {"ResourceWarning", {"Warning", nullptr, nullptr, nullptr}, false, 0, -1},
    {"RuntimeError", {"Exception", nullptr, nullptr, nullptr}, false, 0, -1},
    {"RuntimeWarning", {"Warning", nullptr, nullptr, nullptr}, false, 0, -1},
    {"StopAsyncIteration", {"Exception", nullptr, nullptr, nullptr}, false, 0, -1},
    {"StopIteration", {"Exception", nullptr, nullptr, nullptr}, false, 0, -1},
    {"SyntaxError", {"Exception", nullptr, nullptr, nullptr}, false, 0, -1},
    {"SyntaxWarning", {"Warning", nullptr, nullptr, nullptr}, false, 0, -1},
    {"SystemError", {"Exception", nullptr, nullptr, nullptr}, false, 0, -1},
    {"SystemExit", {"BaseException", nullptr, nullptr, nullptr}, false, 0, -1},
    {"TabError", {"IndentationError", nullptr, nullptr, nullptr}, false, 0, -1},
    {"TimeoutError", {"OSError", nullptr, nullptr, nullptr}, false, 0, -1},
    {"TypeError", {"Exception", nullptr, nullptr, nullptr}, false, 0, -1},
    {"UnboundLocalError", {"NameError", nullptr, nullptr, nullptr}, false, 0, -1},
    {"UnicodeDecodeError", {"UnicodeError", nullptr, nullptr, nullptr}, false, 5, 5},
    {"UnicodeEncodeError", {"UnicodeError", nullptr, nullptr, nullptr}, false, 5, 5},
    {"UnicodeError", {"ValueError", nullptr, nullptr, nullptr}, false, 0, -1},
    {"UnicodeTranslateError", {"UnicodeError", nullptr, nullptr, nullptr}, false, 4, 4},
    {"UnicodeWarning", {"Warning", nullptr, nullptr, nullptr}, false, 0, -1},
    {"UserWarning", {"Warning", nullptr, nullptr, nullptr}, false, 0, -1},
    {"ValueError", {"Exception", nullptr, nullptr, nullptr}, false, 0, -1},
    {"Warning", {"Exception", nullptr, nullptr, nullptr}, false, 0, -1},
    {"WindowsError", {"Exception", nullptr, nullptr, nullptr}, false, 0, -1},
    {"ZeroDivisionError", {"ArithmeticError", nullptr, nullptr, nullptr}, false, 0, -1},
    {"bool", {"int", nullptr, nullptr, nullptr}, false, 0, 1},
    {"bytearray", {nullptr, nullptr, nullptr, nullptr}, false, 0, -1},
    {"bytes", {nullptr, nullptr, nullptr, nullptr}, false, 0, -1},
    {"classmethod", {nullptr, nullptr, nullptr, nullptr}, true, 1, 1},
    {"complex", {nullptr, nullptr, nullptr, nullptr}, false, 0, -1},
    {"dict", {nullptr, nullptr, nullptr, nullptr}, true, 0, -1},
    {"enumerate", {nullptr, nullptr, nullptr, nullptr}, true, 1, 2},
    {"filter", {nullptr, nullptr, nullptr, nullptr}, true, 0, -1},
    {"float", {nullptr, nullptr, nullptr, nullptr}, false, 0, 1},
    {"frozenset", {nullptr, nullptr, nullptr, nullptr}, true, 0, -1},
    {"int", {nullptr, nullptr, nullptr, nullptr}, false, 0, -1},
    {"list", {nullptr, nullptr, nullptr, nullptr}, true, 0, -1},
    {"map", {nullptr, nullptr, nullptr, nullptr}, true, 0, -1},
    {"memoryview", {nullptr, nullptr, nullptr, nullptr}, true, 1, 1},
    {"object", {nullptr, nullptr, nullptr, nullptr}, false, 0, 0},
    {"property", {nullptr, nullptr, nullptr, nullptr}, false, 0, 4},
    {"range", {nullptr, nullptr, nullptr, nullptr}, false, 0, -1},
    {"reversed", {nullptr, nullptr, nullptr, nullptr}, true, 0, -1},
    {"set", {nullptr, nullptr, nullptr, nullptr}, true, 0, -1},
    {"slice", {nullptr, nullptr, nullptr, nullptr}, true, 0, -1},
    {"staticmethod", {nullptr, nullptr, nullptr, nullptr}, true, 1, 1},
    {"str", {nullptr, nullptr, nullptr, nullptr}, false, 0, -1},
    {"super", {nullptr, nullptr, nullptr, nullptr}, false, 0, -1},
    {"tuple", {nullptr, nullptr, nullptr, nullptr}, true, 0, 1},
    {"type", {nullptr, nullptr, nullptr, nullptr}, true, 0, -1},
    {"zip", {nullptr, nullptr, nullptr, nullptr}, true, 0, -1},
};

constexpr std::size_t kBuiltinClassCount =
    sizeof(kBuiltinClasses) / sizeof(kBuiltinClasses[0]);

// Names that are not distinct classes at all, but the SAME class object under
// another spelling -- `getattr(builtins, "IOError") is builtins.OSError` is
// True in CPython. A bases[] entry cannot express this: it means is-a, and an
// alias needs is. `EnvironmentError`, `IOError` and `WindowsError` are all
// `OSError` by identity, so both `x: IOError = OSError()` and
// `y: OSError = IOError()` are mypy-clean, and treating them as three
// distinct classes with a shared base would make one of those two directions
// a false TypeError.
//
// Detected by grouping extracted names by id(getattr(builtins, name)); a
// group of more than one name picks its canonical spelling from
// cls.__name__ (verified to be a member of every such group -- the generator
// raises otherwise) and emits the rest as aliases. A name that is already its
// own canonical is never given a row here.
struct BuiltinClassAlias {
    const char* alias;
    const char* canonical;
};

constexpr BuiltinClassAlias kBuiltinClassAliases[] = {
    {"EnvironmentError", "OSError"},
    {"IOError", "OSError"},
    {"WindowsError", "OSError"},
};

constexpr std::size_t kBuiltinClassAliasCount =
    sizeof(kBuiltinClassAliases) / sizeof(kBuiltinClassAliases[0]);

} // namespace cythonpp::domain::semantic

#endif // CYTHONPP_DOMAIN_SEMANTIC_BUILTIN_CLASS_TABLE_H
