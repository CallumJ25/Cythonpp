#ifndef CYTHONPP_DOMAIN_SEMANTIC_BUILTIN_CLASS_TABLE_H
#define CYTHONPP_DOMAIN_SEMANTIC_BUILTIN_CLASS_TABLE_H

#include <cstddef>

namespace cythonpp::domain::semantic {

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
};

constexpr BuiltinClass kBuiltinClasses[] = {
    {"ArithmeticError", {"Exception", nullptr, nullptr, nullptr}, false},
    {"AssertionError", {"Exception", nullptr, nullptr, nullptr}, false},
    {"AttributeError", {"Exception", nullptr, nullptr, nullptr}, false},
    {"BaseException", {nullptr, nullptr, nullptr, nullptr}, false},
    {"BaseExceptionGroup", {"BaseException", nullptr, nullptr, nullptr}, true},
    {"BlockingIOError", {"OSError", nullptr, nullptr, nullptr}, false},
    {"BrokenPipeError", {"ConnectionError", nullptr, nullptr, nullptr}, false},
    {"BufferError", {"Exception", nullptr, nullptr, nullptr}, false},
    {"BytesWarning", {"Warning", nullptr, nullptr, nullptr}, false},
    {"ChildProcessError", {"OSError", nullptr, nullptr, nullptr}, false},
    {"ConnectionAbortedError", {"ConnectionError", nullptr, nullptr, nullptr}, false},
    {"ConnectionError", {"OSError", nullptr, nullptr, nullptr}, false},
    {"ConnectionRefusedError", {"ConnectionError", nullptr, nullptr, nullptr}, false},
    {"ConnectionResetError", {"ConnectionError", nullptr, nullptr, nullptr}, false},
    {"DeprecationWarning", {"Warning", nullptr, nullptr, nullptr}, false},
    {"EOFError", {"Exception", nullptr, nullptr, nullptr}, false},
    {"EncodingWarning", {"Warning", nullptr, nullptr, nullptr}, false},
    {"EnvironmentError", {"Exception", nullptr, nullptr, nullptr}, false},
    {"Exception", {"BaseException", nullptr, nullptr, nullptr}, false},
    {"ExceptionGroup", {"BaseExceptionGroup", "Exception", nullptr, nullptr}, true},
    {"FileExistsError", {"OSError", nullptr, nullptr, nullptr}, false},
    {"FileNotFoundError", {"OSError", nullptr, nullptr, nullptr}, false},
    {"FloatingPointError", {"ArithmeticError", nullptr, nullptr, nullptr}, false},
    {"FutureWarning", {"Warning", nullptr, nullptr, nullptr}, false},
    {"GeneratorExit", {"BaseException", nullptr, nullptr, nullptr}, false},
    {"IOError", {"Exception", nullptr, nullptr, nullptr}, false},
    {"ImportError", {"Exception", nullptr, nullptr, nullptr}, false},
    {"ImportWarning", {"Warning", nullptr, nullptr, nullptr}, false},
    {"IndentationError", {"SyntaxError", nullptr, nullptr, nullptr}, false},
    {"IndexError", {"LookupError", nullptr, nullptr, nullptr}, false},
    {"InterruptedError", {"OSError", nullptr, nullptr, nullptr}, false},
    {"IsADirectoryError", {"OSError", nullptr, nullptr, nullptr}, false},
    {"KeyError", {"LookupError", nullptr, nullptr, nullptr}, false},
    {"KeyboardInterrupt", {"BaseException", nullptr, nullptr, nullptr}, false},
    {"LookupError", {"Exception", nullptr, nullptr, nullptr}, false},
    {"MemoryError", {"Exception", nullptr, nullptr, nullptr}, false},
    {"ModuleNotFoundError", {"ImportError", nullptr, nullptr, nullptr}, false},
    {"NameError", {"Exception", nullptr, nullptr, nullptr}, false},
    {"NotADirectoryError", {"OSError", nullptr, nullptr, nullptr}, false},
    {"NotImplementedError", {"RuntimeError", nullptr, nullptr, nullptr}, false},
    {"OSError", {"Exception", nullptr, nullptr, nullptr}, false},
    {"OverflowError", {"ArithmeticError", nullptr, nullptr, nullptr}, false},
    {"PendingDeprecationWarning", {"Warning", nullptr, nullptr, nullptr}, false},
    {"PermissionError", {"OSError", nullptr, nullptr, nullptr}, false},
    {"ProcessLookupError", {"OSError", nullptr, nullptr, nullptr}, false},
    {"PythonFinalizationError", {"RuntimeError", nullptr, nullptr, nullptr}, false},
    {"RecursionError", {"RuntimeError", nullptr, nullptr, nullptr}, false},
    {"ReferenceError", {"Exception", nullptr, nullptr, nullptr}, false},
    {"ResourceWarning", {"Warning", nullptr, nullptr, nullptr}, false},
    {"RuntimeError", {"Exception", nullptr, nullptr, nullptr}, false},
    {"RuntimeWarning", {"Warning", nullptr, nullptr, nullptr}, false},
    {"StopAsyncIteration", {"Exception", nullptr, nullptr, nullptr}, false},
    {"StopIteration", {"Exception", nullptr, nullptr, nullptr}, false},
    {"SyntaxError", {"Exception", nullptr, nullptr, nullptr}, false},
    {"SyntaxWarning", {"Warning", nullptr, nullptr, nullptr}, false},
    {"SystemError", {"Exception", nullptr, nullptr, nullptr}, false},
    {"SystemExit", {"BaseException", nullptr, nullptr, nullptr}, false},
    {"TabError", {"IndentationError", nullptr, nullptr, nullptr}, false},
    {"TimeoutError", {"OSError", nullptr, nullptr, nullptr}, false},
    {"TypeError", {"Exception", nullptr, nullptr, nullptr}, false},
    {"UnboundLocalError", {"NameError", nullptr, nullptr, nullptr}, false},
    {"UnicodeDecodeError", {"UnicodeError", nullptr, nullptr, nullptr}, false},
    {"UnicodeEncodeError", {"UnicodeError", nullptr, nullptr, nullptr}, false},
    {"UnicodeError", {"ValueError", nullptr, nullptr, nullptr}, false},
    {"UnicodeTranslateError", {"UnicodeError", nullptr, nullptr, nullptr}, false},
    {"UnicodeWarning", {"Warning", nullptr, nullptr, nullptr}, false},
    {"UserWarning", {"Warning", nullptr, nullptr, nullptr}, false},
    {"ValueError", {"Exception", nullptr, nullptr, nullptr}, false},
    {"Warning", {"Exception", nullptr, nullptr, nullptr}, false},
    {"WindowsError", {"Exception", nullptr, nullptr, nullptr}, false},
    {"ZeroDivisionError", {"ArithmeticError", nullptr, nullptr, nullptr}, false},
    {"bool", {"int", nullptr, nullptr, nullptr}, false},
    {"bytearray", {nullptr, nullptr, nullptr, nullptr}, false},
    {"bytes", {nullptr, nullptr, nullptr, nullptr}, false},
    {"classmethod", {nullptr, nullptr, nullptr, nullptr}, true},
    {"complex", {nullptr, nullptr, nullptr, nullptr}, false},
    {"dict", {nullptr, nullptr, nullptr, nullptr}, true},
    {"enumerate", {nullptr, nullptr, nullptr, nullptr}, true},
    {"filter", {nullptr, nullptr, nullptr, nullptr}, true},
    {"float", {nullptr, nullptr, nullptr, nullptr}, false},
    {"frozenset", {nullptr, nullptr, nullptr, nullptr}, true},
    {"int", {nullptr, nullptr, nullptr, nullptr}, false},
    {"list", {nullptr, nullptr, nullptr, nullptr}, true},
    {"map", {nullptr, nullptr, nullptr, nullptr}, true},
    {"memoryview", {nullptr, nullptr, nullptr, nullptr}, true},
    {"object", {nullptr, nullptr, nullptr, nullptr}, false},
    {"property", {nullptr, nullptr, nullptr, nullptr}, false},
    {"range", {nullptr, nullptr, nullptr, nullptr}, false},
    {"reversed", {nullptr, nullptr, nullptr, nullptr}, true},
    {"set", {nullptr, nullptr, nullptr, nullptr}, true},
    {"slice", {nullptr, nullptr, nullptr, nullptr}, true},
    {"staticmethod", {nullptr, nullptr, nullptr, nullptr}, true},
    {"str", {nullptr, nullptr, nullptr, nullptr}, false},
    {"super", {nullptr, nullptr, nullptr, nullptr}, false},
    {"tuple", {nullptr, nullptr, nullptr, nullptr}, true},
    {"type", {nullptr, nullptr, nullptr, nullptr}, true},
    {"zip", {nullptr, nullptr, nullptr, nullptr}, true},
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
