#ifndef CYTHONPP_DOMAIN_SEMANTIC_BUILTIN_OBJECT_MEMBER_TABLE_H
#define CYTHONPP_DOMAIN_SEMANTIC_BUILTIN_OBJECT_MEMBER_TABLE_H

#include <cstddef>

namespace cythonpp::domain::semantic {

// Every name `dir(object)` reports, MINUS any name `mypy --strict` rejects
// when it is read (not called) as a plain attribute off an ordinary
// no-base instance. GENERATED -- do not edit by hand. Regenerate with:
//
//     python scripts/verify_corpus_labels.py --generate-object-member-table
//
// Extracted from Python 3.14.2 on Windows, filtered against mypy 1.18.1 (compiled: yes).
//
// WHY THIS FILE EXISTS. `object`'s own members are modelled nowhere: a plain
// user class (`class Plain: pass`) records no bases at all, so
// ClassTable::inherits_builtin_class's chain walk -- the fix for the sibling
// defect of a class reaching a SEEDED builtin row -- never runs on it, and
// `object` is deliberately excluded from that walk anyway (every class
// conceptually derives from it; see that function's own comment). So
// `p.__class__`, `p.__repr__`, `p.__hash__` and the rest of `object`'s real
// member set were false `TypeError`s on programs both mypy --strict and
// CPython accept and run -- and the identical false positive reaches
// `object()` used directly, since ClassTable seeds `object` itself with no
// member map entries either.
//
// WHY EXTRACTED RATHER THAN CURATED, the same reason builtin_class_table.h
// and builtin_function_table.h both give: a hand-picked list has exactly one
// failure mode -- a forgotten name -- and it is silent.
//
// WHY FILTERED AGAINST MYPY, AND WHY FIVE NAMES OF THE TWENTY-FOUR IN
// `dir(object)` ARE MISSING HERE. `__init__` is read directly:
// `Accessing "__init__" on an instance is unsound, since instance.__init__
// could be from an incompatible subclass  [misc]` under mypy --strict, so the
// union rule already covers it and this compiler must keep reporting there,
// not go silent. `__lt__`, `__le__`, `__gt__` and `__ge__` are a second,
// distinct case: CPython's `object` genuinely carries all four as slot
// wrappers (`p.__lt__` returns one at runtime, no error), but typeshed's
// `object` stub omits them so total ordering is not accidentally assumed --
// mypy's `--strict` verdict on a bare `p.__ge__` read is `Unsupported left
// operand type for ">=" ("Plain")  [operator]`, an error, even though nothing
// is being compared. Both are cases where mypy rejects and CPython accepts,
// so the union rule says reject, and this table must not paper over that by
// including them.
//
// TWO NAMES A PRIOR (WRONG) DRAFT OF THIS FIX INCLUDED, AND WHY THEY ARE NOT
// HERE. `__dict__` and `__module__` are NOT in `dir(object)` at all --
// `object()` itself raises `AttributeError: 'object' object has no attribute
// '__dict__'` at runtime, measured directly. An ordinary subclass instance
// (`Plain()`) does carry both, but that is a property of the CLASS MACHINERY
// (every class without `__slots__` gets an instance `__dict__`, and every
// class gets a `__module__`), not of `object`'s own member set -- and seeding
// them here would make `object().__dict__` a false CLEAN when CPython
// rejects it outright. That is a separate, unmodelled defect (every ordinary
// class implicitly carries `__dict__`/`__module__`/`__weakref__`/
// `__annotations__`), not this one.
//
// THIS FILE CARRIES NO SIGNATURES, deliberately, the same choice
// builtin_function_table.h makes and for the same reason: several of
// object's real signatures use type machinery this model does not have
// (`Self`, overloaded `SupportsIndex`, `type[Self]`), and guessing one wrong
// would be a false claim. A name found here resolves to Unknown, which is
// absorbing, so the access is clean and no type claim is made.

constexpr const char* kObjectMembers[] = {
    "__class__",
    "__delattr__",
    "__dir__",
    "__doc__",
    "__eq__",
    "__format__",
    "__getattribute__",
    "__getstate__",
    "__hash__",
    "__init_subclass__",
    "__ne__",
    "__new__",
    "__reduce__",
    "__reduce_ex__",
    "__repr__",
    "__setattr__",
    "__sizeof__",
    "__str__",
    "__subclasshook__",
};

constexpr std::size_t kObjectMemberCount =
    sizeof(kObjectMembers) / sizeof(kObjectMembers[0]);

} // namespace cythonpp::domain::semantic

#endif // CYTHONPP_DOMAIN_SEMANTIC_BUILTIN_OBJECT_MEMBER_TABLE_H
