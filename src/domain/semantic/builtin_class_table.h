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
// Seven entries are GENERIC in typeshed -- zip, map, filter, enumerate,
// reversed, staticmethod, classmethod. Bare use (`x: zip`) is a mypy type-arg
// error while ours is clean, a recorded direction-(b) miss. But `x: zip[int]`
// is mypy-CLEAN, so AnnotationResolver must report NotImplementedError for a
// subscripted seeded class, never "'zip' is not subscriptable" (Task 9).

// Four is enough: the widest direct-base list in the extraction is 2. The
// generator raises if that ever stops being true.
struct BuiltinClass {
    const char* name;
    const char* bases[4];
};

constexpr BuiltinClass kBuiltinClasses[] = {
    {"ArithmeticError", {"Exception", nullptr, nullptr, nullptr}},
    {"AssertionError", {"Exception", nullptr, nullptr, nullptr}},
    {"AttributeError", {"Exception", nullptr, nullptr, nullptr}},
    {"BaseException", {nullptr, nullptr, nullptr, nullptr}},
    {"BaseExceptionGroup", {"BaseException", nullptr, nullptr, nullptr}},
    {"BlockingIOError", {"OSError", nullptr, nullptr, nullptr}},
    {"BrokenPipeError", {"ConnectionError", nullptr, nullptr, nullptr}},
    {"BufferError", {"Exception", nullptr, nullptr, nullptr}},
    {"BytesWarning", {"Warning", nullptr, nullptr, nullptr}},
    {"ChildProcessError", {"OSError", nullptr, nullptr, nullptr}},
    {"ConnectionAbortedError", {"ConnectionError", nullptr, nullptr, nullptr}},
    {"ConnectionError", {"OSError", nullptr, nullptr, nullptr}},
    {"ConnectionRefusedError", {"ConnectionError", nullptr, nullptr, nullptr}},
    {"ConnectionResetError", {"ConnectionError", nullptr, nullptr, nullptr}},
    {"DeprecationWarning", {"Warning", nullptr, nullptr, nullptr}},
    {"EOFError", {"Exception", nullptr, nullptr, nullptr}},
    {"EncodingWarning", {"Warning", nullptr, nullptr, nullptr}},
    {"EnvironmentError", {"Exception", nullptr, nullptr, nullptr}},
    {"Exception", {"BaseException", nullptr, nullptr, nullptr}},
    {"ExceptionGroup", {"BaseExceptionGroup", "Exception", nullptr, nullptr}},
    {"FileExistsError", {"OSError", nullptr, nullptr, nullptr}},
    {"FileNotFoundError", {"OSError", nullptr, nullptr, nullptr}},
    {"FloatingPointError", {"ArithmeticError", nullptr, nullptr, nullptr}},
    {"FutureWarning", {"Warning", nullptr, nullptr, nullptr}},
    {"GeneratorExit", {"BaseException", nullptr, nullptr, nullptr}},
    {"IOError", {"Exception", nullptr, nullptr, nullptr}},
    {"ImportError", {"Exception", nullptr, nullptr, nullptr}},
    {"ImportWarning", {"Warning", nullptr, nullptr, nullptr}},
    {"IndentationError", {"SyntaxError", nullptr, nullptr, nullptr}},
    {"IndexError", {"LookupError", nullptr, nullptr, nullptr}},
    {"InterruptedError", {"OSError", nullptr, nullptr, nullptr}},
    {"IsADirectoryError", {"OSError", nullptr, nullptr, nullptr}},
    {"KeyError", {"LookupError", nullptr, nullptr, nullptr}},
    {"KeyboardInterrupt", {"BaseException", nullptr, nullptr, nullptr}},
    {"LookupError", {"Exception", nullptr, nullptr, nullptr}},
    {"MemoryError", {"Exception", nullptr, nullptr, nullptr}},
    {"ModuleNotFoundError", {"ImportError", nullptr, nullptr, nullptr}},
    {"NameError", {"Exception", nullptr, nullptr, nullptr}},
    {"NotADirectoryError", {"OSError", nullptr, nullptr, nullptr}},
    {"NotImplementedError", {"RuntimeError", nullptr, nullptr, nullptr}},
    {"OSError", {"Exception", nullptr, nullptr, nullptr}},
    {"OverflowError", {"ArithmeticError", nullptr, nullptr, nullptr}},
    {"PendingDeprecationWarning", {"Warning", nullptr, nullptr, nullptr}},
    {"PermissionError", {"OSError", nullptr, nullptr, nullptr}},
    {"ProcessLookupError", {"OSError", nullptr, nullptr, nullptr}},
    {"PythonFinalizationError", {"RuntimeError", nullptr, nullptr, nullptr}},
    {"RecursionError", {"RuntimeError", nullptr, nullptr, nullptr}},
    {"ReferenceError", {"Exception", nullptr, nullptr, nullptr}},
    {"ResourceWarning", {"Warning", nullptr, nullptr, nullptr}},
    {"RuntimeError", {"Exception", nullptr, nullptr, nullptr}},
    {"RuntimeWarning", {"Warning", nullptr, nullptr, nullptr}},
    {"StopAsyncIteration", {"Exception", nullptr, nullptr, nullptr}},
    {"StopIteration", {"Exception", nullptr, nullptr, nullptr}},
    {"SyntaxError", {"Exception", nullptr, nullptr, nullptr}},
    {"SyntaxWarning", {"Warning", nullptr, nullptr, nullptr}},
    {"SystemError", {"Exception", nullptr, nullptr, nullptr}},
    {"SystemExit", {"BaseException", nullptr, nullptr, nullptr}},
    {"TabError", {"IndentationError", nullptr, nullptr, nullptr}},
    {"TimeoutError", {"OSError", nullptr, nullptr, nullptr}},
    {"TypeError", {"Exception", nullptr, nullptr, nullptr}},
    {"UnboundLocalError", {"NameError", nullptr, nullptr, nullptr}},
    {"UnicodeDecodeError", {"UnicodeError", nullptr, nullptr, nullptr}},
    {"UnicodeEncodeError", {"UnicodeError", nullptr, nullptr, nullptr}},
    {"UnicodeError", {"ValueError", nullptr, nullptr, nullptr}},
    {"UnicodeTranslateError", {"UnicodeError", nullptr, nullptr, nullptr}},
    {"UnicodeWarning", {"Warning", nullptr, nullptr, nullptr}},
    {"UserWarning", {"Warning", nullptr, nullptr, nullptr}},
    {"ValueError", {"Exception", nullptr, nullptr, nullptr}},
    {"Warning", {"Exception", nullptr, nullptr, nullptr}},
    {"WindowsError", {"Exception", nullptr, nullptr, nullptr}},
    {"ZeroDivisionError", {"ArithmeticError", nullptr, nullptr, nullptr}},
    {"bool", {"int", nullptr, nullptr, nullptr}},
    {"bytearray", {nullptr, nullptr, nullptr, nullptr}},
    {"bytes", {nullptr, nullptr, nullptr, nullptr}},
    {"classmethod", {nullptr, nullptr, nullptr, nullptr}},
    {"complex", {nullptr, nullptr, nullptr, nullptr}},
    {"dict", {nullptr, nullptr, nullptr, nullptr}},
    {"enumerate", {nullptr, nullptr, nullptr, nullptr}},
    {"filter", {nullptr, nullptr, nullptr, nullptr}},
    {"float", {nullptr, nullptr, nullptr, nullptr}},
    {"frozenset", {nullptr, nullptr, nullptr, nullptr}},
    {"int", {nullptr, nullptr, nullptr, nullptr}},
    {"list", {nullptr, nullptr, nullptr, nullptr}},
    {"map", {nullptr, nullptr, nullptr, nullptr}},
    {"memoryview", {nullptr, nullptr, nullptr, nullptr}},
    {"object", {nullptr, nullptr, nullptr, nullptr}},
    {"property", {nullptr, nullptr, nullptr, nullptr}},
    {"range", {nullptr, nullptr, nullptr, nullptr}},
    {"reversed", {nullptr, nullptr, nullptr, nullptr}},
    {"set", {nullptr, nullptr, nullptr, nullptr}},
    {"slice", {nullptr, nullptr, nullptr, nullptr}},
    {"staticmethod", {nullptr, nullptr, nullptr, nullptr}},
    {"str", {nullptr, nullptr, nullptr, nullptr}},
    {"super", {nullptr, nullptr, nullptr, nullptr}},
    {"tuple", {nullptr, nullptr, nullptr, nullptr}},
    {"type", {nullptr, nullptr, nullptr, nullptr}},
    {"zip", {nullptr, nullptr, nullptr, nullptr}},
};

constexpr std::size_t kBuiltinClassCount =
    sizeof(kBuiltinClasses) / sizeof(kBuiltinClasses[0]);

} // namespace cythonpp::domain::semantic

#endif // CYTHONPP_DOMAIN_SEMANTIC_BUILTIN_CLASS_TABLE_H
