#!/usr/bin/env python3
"""Developer tool. Regenerates builtin_class_table.h and builtin_function_table.h,
and re-derives corpus labels.

NEVER run by ctest. The test suite must pass with no Python installed, so this
script's output is CHECKED IN and the tests read the checked-in file.

--generate-class-table needs `mypy` on PATH as well as the interpreter: one
field of builtin_class_table.h (accepts_type_arguments) is a claim about
TYPESHED, not about the running interpreter, and the interpreter gives the
wrong answer for it (see that header's own comment). --check-corpus already
needed mypy, so the script's dependency set is unchanged.

Usage:
    python scripts/verify_corpus_labels.py --generate-class-table
    python scripts/verify_corpus_labels.py --generate-function-table
    python scripts/verify_corpus_labels.py --generate-object-member-table
    python scripts/verify_corpus_labels.py --check-corpus     # added in Task 24
    python scripts/verify_corpus_labels.py --check-codegen    # added in Task 12 (codegen slice)
"""

import argparse
import builtins
import collections
import pathlib
import platform
import subprocess
import sys
import tempfile

MAX_BASES = 4

HEADER = """#ifndef CYTHONPP_DOMAIN_SEMANTIC_BUILTIN_CLASS_TABLE_H
#define CYTHONPP_DOMAIN_SEMANTIC_BUILTIN_CLASS_TABLE_H

#include <cstddef>

namespace cythonpp::domain::semantic {{

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
// Extracted from Python {version} on {system}.
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
// interpreter gives the WRONG answer. Measured on Python {version}:
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
struct BuiltinClass {{
    const char* name;
    const char* bases[{max_bases}];

    // False only for a class mypy reports `"X" expects no type arguments`
    // for. See the paragraph above for why this is mypy-derived rather than
    // interpreter-derived, and builtin_class_genericity.h for the one
    // consumer.
    bool accepts_type_arguments;

    // The constructor arity band mypy accepts, derived by probing
    // `NAME(a(), a(), ...)` with `def a() -> Any: ...` so argument TYPES
    // cannot mask an arity verdict, and filtering to the `[call-arg]` code.
    // An overload mismatch is `[call-overload]` and deliberately does NOT
    // count, so an overloaded class (`type`, which accepts exactly {{1, 3}})
    // reads as unbounded -- a missed error, the safe direction, never a
    // false positive.
    int min_args;
    int max_args;
}};

constexpr BuiltinClass kBuiltinClasses[] = {{
{rows}
}};

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
struct BuiltinClassAlias {{
    const char* alias;
    const char* canonical;
}};

constexpr BuiltinClassAlias kBuiltinClassAliases[] = {{
{alias_rows}
}};

constexpr std::size_t kBuiltinClassAliasCount =
    sizeof(kBuiltinClassAliases) / sizeof(kBuiltinClassAliases[0]);

}} // namespace cythonpp::domain::semantic

#endif // CYTHONPP_DOMAIN_SEMANTIC_BUILTIN_CLASS_TABLE_H
"""


def class_names():
    return [
        name
        for name in sorted(dir(builtins))
        if isinstance(getattr(builtins, name), type) and not name.startswith("_")
    ]


def find_aliases(names):
    """Group names by object identity; return sorted (alias, canonical) pairs.

    A group with more than one name is the SAME class object under multiple
    spellings (e.g. IOError is OSError). The canonical spelling is
    cls.__name__, which must itself be one of the group's names -- if it is
    not, the assumption that __name__ picks a member of its own alias group
    is false and generation must stop rather than emit a guess.
    """
    groups = collections.defaultdict(list)
    for name in names:
        groups[id(getattr(builtins, name))].append(name)

    aliases = []
    for group_names in groups.values():
        if len(group_names) <= 1:
            continue
        canonical = getattr(builtins, group_names[0]).__name__
        if canonical not in group_names:
            raise SystemExit(
                f"alias group {group_names} has no member named {canonical!r} "
                f"(cls.__name__); cannot pick a canonical spelling"
            )
        for name in group_names:
            if name != canonical:
                aliases.append((name, canonical))
    aliases.sort()
    return aliases


# The one verdict that means "not generic". Every other mypy verdict on
# `NAME[int]` -- silence, a different arity, a type-var bound complaint --
# means the class DOES take type arguments.
_NOT_GENERIC_MARKER = "expects no type arguments"

# Verdicts that positively confirm genericity. Listed so an UNRECOGNISED
# diagnostic raises instead of being silently read as "generic": a misread
# there would route a genuine `not subscriptable` TypeError to
# NotImplementedError, which is the safe direction but still a missed error.
_GENERIC_MARKERS = (
    "expects 2 type arguments",
    "expects 3 type arguments",
    "Missing type parameters for generic type",
    "must be a subtype of",
)


def accepts_type_arguments(names):
    """name -> bool, by asking `mypy --strict` about `NAME[int]`.

    NOT by asking the interpreter: `hasattr(cls, "__class_getitem__")` and
    `cls[int]` both say False for filter/map/reversed/zip/slice, all of which
    mypy accepts a type argument on. The union rule turns on what mypy says,
    so mypy is what gets asked.
    """
    with tempfile.TemporaryDirectory() as tmpdir:
        probe = pathlib.Path(tmpdir) / "generic_probe.py"
        probe.write_text(
            "".join(f"def f{i}(a: {n}[int]) -> None: pass\n" for i, n in enumerate(names)),
            encoding="utf-8",
        )
        result = subprocess.run(
            ["mypy", "--strict", "--no-color-output", "--no-error-summary", probe.name],
            cwd=tmpdir,
            capture_output=True,
            text=True,
        )
        if result.returncode >= 2:
            raise SystemExit(
                f"mypy --strict exited {result.returncode} on the genericity probe; "
                f"cannot derive accepts_type_arguments\n{result.stdout}{result.stderr}"
            )
        verdicts = collections.defaultdict(list)
        for line in result.stdout.splitlines():
            parts = line.split(":", 2)
            if len(parts) < 3 or not parts[1].isdigit():
                continue
            verdicts[int(parts[1])].append(parts[2].strip())

    generic = {}
    for index, name in enumerate(names):
        messages = verdicts.get(index + 1, [])
        errors = [m for m in messages if m.startswith("error:")]
        if not errors:
            generic[name] = True
            continue
        if any(_NOT_GENERIC_MARKER in m for m in errors):
            generic[name] = False
            continue
        if all(any(marker in m for marker in _GENERIC_MARKERS) for m in errors):
            generic[name] = True
            continue
        raise SystemExit(
            f"unrecognised mypy verdict for {name}[int]; refusing to guess "
            f"accepts_type_arguments: {errors}"
        )
    return generic


_MAX_PROBED_ARITY = 8


def constructor_arity(names):
    """name -> (min_args, max_args), by asking `mypy --strict` directly.

    Argument TYPES are neutralised with `Any` rather than chosen per name: a
    sweep using a literal value makes mypy reject on [arg-type] grounds for
    some names, which scores an arity defect as "both report" and hides it.
    Only [call-arg] counts -- an overload mismatch is [call-overload], and
    counting it would make the accepted set non-contiguous (`type` accepts
    exactly {1, 3}) and risks reporting where the mismatch is really about
    types. Excluding it leaves overloaded classes unbounded, which is a
    missed error -- the safe direction -- never a false positive.

    mypy alone is a sufficient oracle here: under the union rule, silence
    where mypy accepts can never be a false positive, and reporting where
    mypy rejects can never be unsound.

    max_args is -1 (kUnboundedArity) when the accepted set reaches
    _MAX_PROBED_ARITY, meaning this probe found no upper bound at all.
    """
    accepted = {name: set() for name in names}
    with tempfile.TemporaryDirectory() as tmpdir:
        for k in range(_MAX_PROBED_ARITY + 1):
            args = ", ".join(["a()"] * k)
            lines = ["from typing import Any", "def a() -> Any: ..."]
            first = len(lines) + 1
            lines.extend(f"{name}({args})" for name in names)
            probe = pathlib.Path(tmpdir) / f"arity_{k}.py"
            probe.write_text("\n".join(lines) + "\n", encoding="utf-8")
            result = subprocess.run(
                ["mypy", "--strict", "--no-color-output", "--no-error-summary", probe.name],
                cwd=tmpdir,
                capture_output=True,
                text=True,
            )
            if result.returncode >= 2:
                raise SystemExit(
                    f"mypy --strict exited {result.returncode} on the arity probe; "
                    f"cannot derive constructor arities\n{result.stdout}{result.stderr}"
                )
            rejected = set()
            for line in result.stdout.splitlines():
                parts = line.split(":", 2)
                if len(parts) < 3 or not parts[1].isdigit():
                    continue
                if "[call-arg]" in parts[2]:
                    rejected.add(int(parts[1]))
            for index, name in enumerate(names):
                if first + index not in rejected:
                    accepted[name].add(k)

    # An early draft of this probe invoked `python -m mypy`, which does not
    # exist on the reference machine: it exited 0 with EMPTY output and
    # scored EVERY arity as accepted, for all names, silently. These
    # assertions are what make that failure loud instead of a silently
    # vacuous table.
    if 1 not in accepted["bool"] or _MAX_PROBED_ARITY in accepted["bool"]:
        raise SystemExit(
            "vacuous arity probe: bool must accept 1 argument and reject "
            f"{_MAX_PROBED_ARITY}; got {sorted(accepted['bool'])}"
        )

    arities = {}
    for name in names:
        ks = accepted[name]
        if not ks:
            raise SystemExit(f"{name} accepts no arity in 0..{_MAX_PROBED_ARITY}")
        low, high = min(ks), max(ks)
        if ks != set(range(low, high + 1)):
            raise SystemExit(
                f"{name} has a NON-CONTIGUOUS accepted arity set {sorted(ks)}. "
                "The two-int band this table stores cannot represent it; see "
                "the header comment before widening the representation."
            )
        if high == _MAX_PROBED_ARITY and low != 0:
            # The header states this as MEASURED FACT, not merely assumed:
            # "every unbounded row also accepts ZERO arguments ... there is
            # no row needing 'min 3, max unbounded'". kUnboundedArity is a
            # single sentinel standing in for BOTH ends of the band, and
            # class_table.cpp's SEEDING stores both min_args and max_args
            # unconditionally -- it is CONSUMPTION (constructor_check's
            # has_unbounded_arity check) that looks at max_args alone and
            # never asks what min_args said. So a row accepted at, say,
            # {2..8} would seed correctly but still be treated as wholly
            # unconstrained wherever the band is read, silently turning a
            # real lower bound of 2 into unconditional silence. Latent today
            # (every one of the 83 unbounded rows measured 2026-09-12 really
            # does start at 0), but a future typeshed change introducing one
            # must fail loudly here rather than be silently mis-stored.
            raise SystemExit(
                f"{name} is unbounded above (accepts {_MAX_PROBED_ARITY}) but its minimum "
                f"is {low}, not 0. The kUnboundedArity sentinel cannot represent a nonzero "
                "lower bound; widen the representation before seeding this row."
            )
        arities[name] = (low, -1 if high == _MAX_PROBED_ARITY else high)
    return arities


def generate_class_table() -> str:
    names = class_names()
    known = set(names)
    generic = accepts_type_arguments(names)
    arities = constructor_arity(names)
    rows = []
    for name in names:
        cls = getattr(builtins, name)
        bases = [
            base.__name__
            for base in cls.__bases__
            if base.__name__ in known and base.__name__ != "object"
        ]
        if len(bases) > MAX_BASES:
            raise SystemExit(
                f"{name} has {len(bases)} direct bases; bump MAX_BASES "
                f"and struct BuiltinClass::bases"
            )
        padded = bases + [None] * (MAX_BASES - len(bases))
        rendered = ", ".join("nullptr" if b is None else f'"{b}"' for b in padded)
        flag = "true" if generic[name] else "false"
        low, high = arities[name]
        rows.append(f'    {{"{name}", {{{rendered}}}, {flag}, {low}, {high}}},')

    alias_rows = [
        f'    {{"{alias}", "{canonical}"}},' for alias, canonical in find_aliases(names)
    ]

    return HEADER.format(
        version=sys.version.split()[0],
        system=platform.system(),
        max_bases=MAX_BASES,
        rows="\n".join(rows),
        alias_rows="\n".join(alias_rows),
    )


FUNCTION_HEADER = """#ifndef CYTHONPP_DOMAIN_SEMANTIC_BUILTIN_FUNCTION_TABLE_H
#define CYTHONPP_DOMAIN_SEMANTIC_BUILTIN_FUNCTION_TABLE_H

#include <cstddef>

namespace cythonpp::domain::semantic {{

// Every name in Python's `builtins` module that is callable and is NOT a
// class. GENERATED -- do not edit by hand. Regenerate with:
//
//     python scripts/verify_corpus_labels.py --generate-function-table
//
// Extracted from Python {version} on {system}.
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
// reports nothing, and `y: type = len` (which mypy rejects with an
// incompatible-assignment error) becomes a missed error. Measured, some of
// these are missed by BOTH oracles, not just mypy: `g = len; g(1, 2, 3)`
// raises `TypeError: len() takes exactly one argument (3 given)` under
// CPython as well as two mypy errors, and cythonpp reports nothing for
// either. The asymmetry worth knowing: a CALL to an unmodelled builtin still
// draws a loud NotImplementedError (see builtin_call_table.h), but a VALUE
// reference to that same builtin draws nothing at all here, even though
// neither can actually be code-generated. Modelling real signatures for
// builtin functions is separate work; builtin_call_table.h is where a
// modelled one goes.

constexpr const char* kBuiltinFunctions[] = {{
{rows}
}};

constexpr std::size_t kBuiltinFunctionCount =
    sizeof(kBuiltinFunctions) / sizeof(kBuiltinFunctions[0]);

}} // namespace cythonpp::domain::semantic

#endif // CYTHONPP_DOMAIN_SEMANTIC_BUILTIN_FUNCTION_TABLE_H
"""


def function_names():
    """Callable builtins that are NOT classes -- the exact complement of
    class_names(), so no name can appear in both tables."""
    return [
        name
        for name in sorted(dir(builtins))
        if not name.startswith("_")
        and callable(getattr(builtins, name))
        and not isinstance(getattr(builtins, name), type)
    ]


def generate_function_table() -> str:
    rows = [f'    "{name}",' for name in function_names()]
    return FUNCTION_HEADER.format(
        version=sys.version.split()[0],
        system=platform.system(),
        rows="\n".join(rows),
    )


OBJECT_MEMBER_HEADER = """#ifndef CYTHONPP_DOMAIN_SEMANTIC_BUILTIN_OBJECT_MEMBER_TABLE_H
#define CYTHONPP_DOMAIN_SEMANTIC_BUILTIN_OBJECT_MEMBER_TABLE_H

#include <cstddef>

namespace cythonpp::domain::semantic {{

// Every name `dir(object)` reports, MINUS any name `mypy --strict` rejects
// when it is read (not called) as a plain attribute off an ordinary
// no-base instance. GENERATED -- do not edit by hand. Regenerate with:
//
//     python scripts/verify_corpus_labels.py --generate-object-member-table
//
// Extracted from Python {version} on {system}, filtered against {mypy_version}.
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

constexpr const char* kObjectMembers[] = {{
{rows}
}};

constexpr std::size_t kObjectMemberCount =
    sizeof(kObjectMembers) / sizeof(kObjectMembers[0]);

}} // namespace cythonpp::domain::semantic

#endif // CYTHONPP_DOMAIN_SEMANTIC_BUILTIN_OBJECT_MEMBER_TABLE_H
"""


def object_member_names():
    """Every `dir(object)` name that mypy --strict does not reject when read
    (never called) as a plain attribute off a fresh no-base class instance.

    One probe file, one name per line, so a single mypy invocation settles
    all twenty-four at once -- the same one-shot-probe idiom
    accepts_type_arguments and constructor_arity already use, for the same
    reason: a per-name subprocess would be slower and no more precise.
    """
    names = sorted(dir(object))
    with tempfile.TemporaryDirectory() as tmpdir:
        probe = pathlib.Path(tmpdir) / "object_member_probe.py"
        lines = ["class Plain:", "    pass", "", "p = Plain()"]
        first = len(lines) + 1
        lines.extend(f"print(p.{name})" for name in names)
        probe.write_text("\n".join(lines) + "\n", encoding="utf-8")
        result = subprocess.run(
            ["mypy", "--strict", "--no-color-output", "--no-error-summary", probe.name],
            cwd=tmpdir,
            capture_output=True,
            text=True,
        )
        if result.returncode >= 2:
            raise SystemExit(
                f"mypy --strict exited {result.returncode} on the object-member probe; "
                f"cannot derive object member names\n{result.stdout}{result.stderr}"
            )
        rejected_lines = set()
        for line in result.stdout.splitlines():
            parts = line.split(":", 2)
            if len(parts) < 3 or not parts[1].isdigit():
                continue
            rejected_lines.add(int(parts[1]))

    # The `python -m mypy` vacuous-invocation trap constructor_arity's own
    # comment records (exit 0, empty output, every arity silently accepted)
    # has an identical failure mode here: an empty rejected_lines set reads as
    # "mypy accepted all 24", indistinguishable from a genuine clean verdict.
    # __init__ is the known-rejected control -- assert it actually rejected.
    init_line = first + names.index("__init__")
    if init_line not in rejected_lines:
        raise SystemExit(
            "vacuous object-member probe: mypy must reject a bare `p.__init__` read "
            f"(misc: unsound instance access); got no error on line {init_line}. Is the "
            "standalone `mypy` executable on PATH, not `python -m mypy`?"
        )

    accepted = [
        name for index, name in enumerate(names) if (first + index) not in rejected_lines
    ]
    return accepted


def _mypy_version() -> str:
    result = subprocess.run(["mypy", "--version"], capture_output=True, text=True)
    return result.stdout.strip() or result.stderr.strip()


def generate_object_member_table() -> str:
    rows = [f'    "{name}",' for name in object_member_names()]
    return OBJECT_MEMBER_HEADER.format(
        version=sys.version.split()[0],
        system=platform.system(),
        mypy_version=_mypy_version(),
        rows="\n".join(rows),
    )


def corpus_dir() -> pathlib.Path:
    # Relative to this script's own location, never a hardcoded absolute
    # path -- this file is tracked, so it must work on any machine's clone.
    return pathlib.Path(__file__).resolve().parent.parent / "test_files" / "semantic"


def _mypy_blocked_on_sample(sample_name: str, result) -> bool:
    """True when mypy's exit 2 is a BLOCKING error about this sample.

    mypy uses exit 2 for two unrelated things. A blocking source error --
    `Duplicate argument "x" in function definition`, `"break" outside loop` --
    is a real verdict: its semantic analyzer reported on the file and then
    stopped before type checking, printing `<file>:<line>: error: ...` plus
    `(errors prevented further checking)`. A crash or fatal invocation error
    is not a verdict at all, and must never be allowed to "confirm" an
    '# mypy: error' label, because then a broken mypy would confirm every one
    of them.

    The discriminator is "did mypy report a diagnostic against THIS FILE":
    a `<sample>:<line>: error:` line on stdout. An INTERNAL ERROR is excluded
    explicitly, because mypy formats those as a file-and-line diagnostic too
    (`file.py:3: error: INTERNAL ERROR --`) and would otherwise slip through.

    A `[syntax]` error is excluded for the same reason, and this exclusion is
    the round-5 tightening: without it an UNPARSEABLE sample confirmed its
    `# mypy: error` label, where `613a460` had failed it loudly as CRASHED.
    The two families separate cleanly, measured 2026-09-11 with mypy 1.18.1:

        def f(                       -> `:1: error: '(' was never closed  [syntax]`   exit 2
        x = (1                       -> `:2: error: '(' was never closed  [syntax]`   exit 2
        a bad indent                 -> `:3: error: Unexpected indent  [syntax]`      exit 2
        def g(x: int, x: str)        -> `:1: error: Duplicate argument "x" in
                                          function definition`   exit 2, NO code
        a module-level `break`       -> `:1: error: "break" outside loop`  exit 2, NO code

    So every shape the exit-2 loosening exists for is codeless, and every
    shape it must not bless carries `[syntax]`. A corrupt sample would still
    go red in `ctest` (semantic_corpus_test.cpp reads a missing `# cythonpp:`
    line as "zero diagnostics", and cythonpp reports SyntaxError for one), but
    this script is the oracle every `# mypy:` label rests on, so it should not
    be the harness that has to catch it.
    """
    combined = result.stdout + result.stderr
    if "INTERNAL ERROR" in combined:
        return False
    prefix = sample_name + ":"
    blocked = False
    for line in result.stdout.splitlines():
        stripped = line.strip()
        if not stripped.startswith(prefix):
            continue
        rest = stripped[len(prefix):]
        line_number, separator, tail = rest.partition(":")
        if not separator or not line_number.isdigit():
            continue
        message = tail.strip()
        if not message.startswith("error:"):
            continue
        if message.endswith("[syntax]"):
            # Decisive, not merely unconvincing: a sample mypy cannot parse is
            # not a verdict on the sample's TYPES at all, so it must not
            # confirm anything. Returned rather than recorded, so one syntax
            # error outweighs any number of other diagnostics.
            return False
        blocked = True
    return blocked


def _is_mypy_error_line(line: str) -> bool:
    """True for exactly '# mypy: error' or '# mypy: error ' + freeform detail.

    Mirrors semantic_corpus_test.cpp's is_mypy_error_line. A bare prefix
    match (`startswith("# mypy: error")` with no trailing-space requirement)
    would also accept a typo like '# mypy: errorX' -- and 'error' is the
    label that disables the C++ harness's invariant guard, so a lenient
    match there would sit on exactly the wrong side. Kept in lockstep with
    the C++ version even though this script only branches on clean-vs-not,
    same as that one does.
    """
    return line == "# mypy: error" or line.startswith("# mypy: error ")


# One sample's parsed "# cpython:" claim. `kind` is "clean" or "error"; for
# "clean" the other two fields are None. Mirrors semantic_corpus_test.cpp's
# CPythonLabel -- except that THIS side actually re-derives it by running the
# sample, which is the whole point of the label existing.
CPythonLabel = collections.namedtuple("CPythonLabel", "kind exception message")

Header = collections.namedtuple("Header", "mypy_clean cpython header_count")


def _parse_cpython_label(stripped: str) -> CPythonLabel:
    """Parses '# cpython: clean' or '# cpython: error <Exc>: <message>'.

    Mirrors semantic_corpus_test.cpp's parse_cpython_line. This is the label
    that EXEMPTS a '# mypy: clean' sample from the C++ harness's invariant
    guard, so a typo must fail loudly rather than quietly half-apply.
    """
    rest = stripped[len("# cpython:") :].lstrip(" ")
    if not rest:
        raise ValueError(f"'# cpython:' line has nothing after the prefix: {stripped}")
    if rest == "clean":
        return CPythonLabel("clean", None, None)
    if not rest.startswith("error "):
        raise ValueError(
            "expected '# cpython: clean' or '# cpython: error <ExceptionType>: <message>', "
            f"got: {stripped}"
        )
    detail = rest[len("error ") :]
    exception, separator, message = detail.partition(": ")
    if not separator or not exception or not message:
        raise ValueError(
            f"'# cpython: error' needs '<ExceptionType>: <message>' after it, got: {stripped}"
        )
    return CPythonLabel("error", exception, message)


def parse_header(lines) -> Header:
    """Returns (mypy_clean, cpython, header_line_count) for a sample's header.

    Mirrors semantic_corpus_test.cpp's parse_labels: the header is line 1
    ("# mypy: clean" or "# mypy: error ...") plus every contiguous
    "# cythonpp: ..." line and at most one "# cpython: ..." line right after
    it. `cpython` is None for a sample that makes no claim about CPython,
    which is almost all of them. Raises ValueError for anything
    that doesn't match -- a label this cannot parse must fail loudly, not be
    silently skipped.

    Once the header ends (the first line that is none of those), the REST of
    the lines are still scanned -- not ignored -- purely to catch a
    "# cythonpp:"/"# cpython:" line placed below the header by mistake.
    Silently dropping such a line would shrink the expected-diagnostics list
    without a trace, or drop a CPython claim entirely,
    which is exactly the "must fail loudly, not be skipped" doctrine this
    parser claims to follow, so a detached label is a parse error, not a
    no-op. Mirrors the C++ `in_header` flag exactly.
    """
    if not lines:
        raise ValueError("file is empty or has no '# mypy:' header line")

    first = lines[0].rstrip("\r\n")
    if first == "# mypy: clean":
        mypy_clean = True
    elif _is_mypy_error_line(first):
        mypy_clean = False
    else:
        raise ValueError(
            f"first line must be '# mypy: clean' or '# mypy: error ...', got: {first!r} "
            "(if this looks identical to a valid header, check for trailing whitespace -- "
            "an editor auto-save is a common cause, and '# mypy: clean' must match exactly)"
        )

    header_count = 1
    in_header = True
    cpython = None
    for line in lines[1:]:
        stripped = line.rstrip("\r\n")
        is_cythonpp_line = stripped.startswith("# cythonpp:")
        is_cpython_line = stripped.startswith("# cpython:")
        if in_header and is_cythonpp_line:
            header_count += 1
            continue
        if in_header and is_cpython_line:
            if cpython is not None:
                raise ValueError(
                    "more than one '# cpython:' line; a sample states CPython's verdict at "
                    f"most once: {stripped}"
                )
            cpython = _parse_cpython_label(stripped)
            header_count += 1
            continue
        if in_header:
            in_header = False  # First non-header line: the header is over.
        # Past the header now (possibly as of this very line). Such a line
        # here is detached from the header block and would otherwise vanish
        # without a trace.
        if is_cythonpp_line or is_cpython_line:
            raise ValueError(
                "label line found below the header, detached from the leading "
                "'# mypy:'/'# cythonpp:'/'# cpython:' block -- move it up next to the other "
                f"label lines: {stripped}"
            )
    return Header(mypy_clean, cpython, header_count)


def strip_header(lines, header_count):
    """Blanks out the header lines but keeps their line numbers.

    A mypy error against the stripped copy still points at the same line as
    it would in the original file, useful when diagnosing a MISMATCH. It also
    sidesteps mypy's own inline-config parsing of a literal "# mypy: ..."
    comment -- left in place, that line is read as a per-file mypy option
    (e.g. "clean" or "error operator" as flag names) and mypy reports a
    spurious "Unrecognized option" error instead of checking the code below
    it.
    """
    return ["\n"] * header_count + lines[header_count:]


def _check_cpython_label(sample_name: str, tmp_file: pathlib.Path, tmpdir: str, label):
    """Runs the sample under CPython and compares the result to its label.

    Returns (status, detail): status is "OK", "MISMATCH" or "CRASHED", and
    detail is a human-readable explanation for the two failing ones.

    Why this exists at all: a '# cpython: error ...' label is what exempts a
    '# mypy: clean' sample from the C++ harness's invariant guard, and that
    harness cannot run Python (ctest must pass on a machine with no Python
    installed). So the CPython claim -- the ONLY load-bearing assertion in
    such a sample -- would otherwise be the one thing nothing ever verifies.

    Run one file per invocation with cwd set to the same throwaway temp
    directory mypy uses, so a sample that writes files cannot touch the repo,
    and against the HEADER-STRIPPED copy so a traceback's line numbers still
    match the original file. Matching is on the traceback's LAST non-empty
    stderr line, which is exactly '<ExceptionType>: <message>' for an
    uncaught exception.
    """
    try:
        result = subprocess.run(
            [sys.executable, str(tmp_file)],
            cwd=tmpdir,
            capture_output=True,
            text=True,
            timeout=60,
        )
    except subprocess.TimeoutExpired:
        return "CRASHED", f"{sample_name}: running it under CPython timed out after 60s"

    last_line = ""
    for line in reversed(result.stderr.splitlines()):
        if line.strip():
            last_line = line.strip()
            break

    if label.kind == "clean":
        if result.returncode == 0:
            return "OK", ""
        return "MISMATCH", (
            f"labelled '# cpython: clean' but CPython exited {result.returncode}:\n"
            f"{result.stdout}{result.stderr}"
        )

    expected = f"{label.exception}: {label.message}"
    if result.returncode == 0:
        return "MISMATCH", (
            f"labelled '# cpython: error {expected}' but CPython ran it cleanly "
            f"(exit 0):\n{result.stdout}"
        )
    if last_line != expected:
        return "MISMATCH", (
            f"labelled '# cpython: error {expected}' but CPython raised "
            f"{last_line!r}:\n{result.stderr}"
        )
    return "OK", ""


def check_corpus() -> int:
    """Runs mypy --strict on every test_files/semantic/*.py sample, and
    CPython on every sample carrying a '# cpython:' label, then reports
    whether each header matches reality.

    Developer-run only -- NEVER invoked by ctest, which must stay hermetic
    (no Python, no network, no shelling out). This is the only thing in the
    project that actually confirms a '# mypy: clean' or '# cpython: ...'
    label is true rather than asserted from belief.

    Both tools are run with their cwd set to a fresh TemporaryDirectory, so
    any .mypy_cache, __pycache__ or file a sample writes never touches the
    repository at all.
    """
    samples = sorted(corpus_dir().glob("*.py"))
    if not samples:
        print(f"no *.py files found under {corpus_dir()}", file=sys.stderr)
        return 1

    failures = []
    cpython_checked = 0
    with tempfile.TemporaryDirectory() as tmpdir:
        tmpdir_path = pathlib.Path(tmpdir)
        for sample in samples:
            lines = sample.read_text(encoding="utf-8").splitlines(keepends=True)
            try:
                header = parse_header(lines)
            except ValueError as exc:
                print(f"MALFORMED  {sample.name}: {exc}")
                failures.append(f"{sample}: {exc}")
                continue
            mypy_clean = header.mypy_clean

            tmp_file = tmpdir_path / sample.name
            tmp_file.write_text(
                "".join(strip_header(lines, header.header_count)), encoding="utf-8"
            )

            # The CPython half, for the samples that claim anything about it.
            # Checked BEFORE mypy and reported separately, so a sample can
            # fail one half and still have the other half's verdict printed.
            if header.cpython is not None:
                cpython_checked += 1
                status, detail = _check_cpython_label(
                    sample.name, tmp_file, tmpdir, header.cpython
                )
                if status == "OK":
                    print(f"OK         {sample.name}  (cpython label: {header.cpython.kind})")
                else:
                    print(f"{status:<10} {sample.name}  (cpython label: {header.cpython.kind})")
                    failures.append(f"{sample}: {detail}")

            try:
                result = subprocess.run(
                    ["mypy", "--strict", str(tmp_file)],
                    cwd=tmpdir,
                    capture_output=True,
                    text=True,
                )
            except FileNotFoundError:
                print("mypy not found on PATH", file=sys.stderr)
                return 1

            # mypy's exit codes are 0 (clean), 1 (type errors found), and 2
            # for BOTH a crash/fatal error (bad flags, an internal exception)
            # AND a BLOCKING source error -- one its semantic analyzer
            # reports before type checking can start, such as `Duplicate
            # argument "x" in function definition`. The second of those IS a
            # real verdict about the sample, and the union rule needs it:
            # CPython refuses to compile such a file at all, so a sample
            # pinning one is among the most valuable a corpus can hold.
            #
            # Treating every exit 2 as "error" would let a crashed mypy
            # happily "validate" every '# mypy: error' label without ever
            # having checked the code, so the two cases are told apart by
            # whether mypy actually reported on THIS FILE -- see
            # _mypy_blocked_on_sample.
            if result.returncode >= 2 and not _mypy_blocked_on_sample(sample.name, result):
                print(
                    f"CRASHED    {sample.name}: mypy --strict exited {result.returncode} "
                    "(crash or fatal error, not a clean/error verdict)"
                )
                failures.append(
                    f"{sample}: mypy --strict crashed (exit {result.returncode}), this is not "
                    f"a clean/error signal:\n{result.stdout}{result.stderr}"
                )
                continue

            mypy_actually_clean = result.returncode == 0
            label = "clean" if mypy_clean else "error"
            actual = "clean" if mypy_actually_clean else "error"
            if mypy_actually_clean == mypy_clean:
                print(f"OK         {sample.name}  (mypy label: {label})")
            else:
                print(f"MISMATCH   {sample.name}  (mypy label: {label}, mypy says: {actual})")
                failures.append(
                    f"{sample}: labelled '# mypy: {label}' but mypy --strict says "
                    f"'{actual}':\n{result.stdout}{result.stderr}"
                )

    if failures:
        print("\nFAILURES:", file=sys.stderr)
        for failure in failures:
            print(f"  - {failure}", file=sys.stderr)
        return 1

    print(
        f"\nAll {len(samples)} sample(s) match their '# mypy:' header, and all "
        f"{cpython_checked} sample(s) carrying a '# cpython:' header match that too."
    )
    return 0


def codegen_corpus_dir() -> pathlib.Path:
    # Mirrors corpus_dir() above: relative to this script's own location,
    # never a hardcoded absolute path.
    return pathlib.Path(__file__).resolve().parent.parent / "test_files" / "codegen"


def parse_stdout_label(lines):
    """Parses the leading '# stdout:' block of a codegen sample.

    Mirrors codegen_corpus_test.cpp's parse_label exactly: the FIRST line
    must read exactly '# stdout:', and every following '# '-prefixed line
    (or a bare '#', for a blank expected output line) is one more expected
    output line. The block ends at the first line that is neither -- where
    the real Python source begins. If the two parsers ever disagreed about
    a sample, this whole arm would prove nothing, so this must track the
    C++ version's semantics precisely rather than approximate them.

    Returns (expected_lines, header_line_count). Raises ValueError for
    anything that doesn't match -- a label this cannot parse must fail
    loudly, not be silently skipped, the same doctrine parse_header (the
    semantic-corpus parser above) already follows.
    """
    if not lines:
        raise ValueError("file is empty; expected a '# stdout:' header")

    first = lines[0].rstrip("\r\n")
    if first != "# stdout:":
        raise ValueError(f"first line must be exactly '# stdout:', got: {first!r}")

    expected = []
    header_count = 1
    for line in lines[1:]:
        stripped = line.rstrip("\r\n")
        if stripped == "#":
            expected.append("")
            header_count += 1
            continue
        if stripped.startswith("# "):
            expected.append(stripped[2:])
            header_count += 1
            continue
        break  # First non-label line: the header is over.
    return expected, header_count


def _check_codegen_label(sample_name: str, tmp_file: pathlib.Path, tmpdir: str, expected_lines):
    """Runs one codegen sample under CPython and compares stdout to its label.

    Returns (status, detail): status is "OK", "MISMATCH" or "CRASHED", and
    detail is a human-readable explanation for the two failing ones. Shape
    mirrors _check_cpython_label above: run one file per invocation, cwd set
    to the shared throwaway tmpdir so a sample cannot touch the repo or
    import anything from the working tree, against the header-stripped copy
    so a traceback's line numbers still match the original file.

    A nonzero exit code, OR any stderr output even alongside exit 0, is
    reported as CRASHED rather than compared: every codegen sample must be a
    program CPython runs to completion, and a bad sample's partial stdout
    proves nothing about its label.

    TRAILING-NEWLINE RULE, chosen to match codegen_corpus_test.cpp's
    check_sample() byte-for-byte rather than approximate it: the expected
    string is built by appending '\\n' after EVERY label line, including the
    last one (exactly what that function's `expected += line; expected +=
    '\\n';` loop does), and compared against CPython's raw, UNSTRIPPED
    stdout. No trailing newline is added to or stripped from the actual
    output on either side. This works because every sample's last statement
    is a `print(...)` call, which itself ends in '\\n' -- so the byte-exact
    expectation and "one print per label line" coincide, and stripping
    either side would make this arm accept a label the C++ harness would
    reject (or vice versa).
    """
    try:
        result = subprocess.run(
            [sys.executable, str(tmp_file)],
            cwd=tmpdir,
            capture_output=True,
            text=True,
            timeout=60,
        )
    except subprocess.TimeoutExpired:
        return "CRASHED", f"{sample_name}: running it under CPython timed out after 60s"

    if result.returncode != 0:
        return "CRASHED", (
            f"{sample_name}: CPython exited {result.returncode}, not a program that runs to "
            f"completion -- a codegen sample must always exit 0:\n{result.stdout}{result.stderr}"
        )
    if result.stderr:
        return "CRASHED", (
            f"{sample_name}: CPython wrote to stderr even though it exited 0, which a clean "
            f"codegen sample must never do:\n{result.stderr}"
        )

    expected = "".join(line + "\n" for line in expected_lines)
    if result.stdout != expected:
        return "MISMATCH", (
            f"{sample_name}: labelled '# stdout:' does not match CPython's real stdout\n"
            f"  expected: {expected!r}\n"
            f"  actual:   {result.stdout!r}"
        )
    return "OK", ""


def check_codegen_labels() -> int:
    """Runs every test_files/codegen/*.py sample under real CPython and
    confirms its '# stdout:' label matches.

    This is the other half of the asymmetry codegen_corpus_test.cpp's own
    header comment names explicitly: ctest's EverySampleMatchesItsLabel
    proves the emitted C++ program's stdout matches the RECORDED label, on
    every run, with no Python involved at all. It does NOT prove the
    recorded label matches CPython -- that rests entirely on a developer
    having actually run `python <file>` and pasted the real output in by
    hand. This function is what actually checks that, the same way
    check_corpus()'s CPython arm (_check_cpython_label) is what actually
    checks a '# cpython:' label in the semantic corpus.

    Developer-run only -- NEVER invoked by ctest, which must stay hermetic
    (no Python, no network, no shelling out). Every sample is run with its
    cwd set to a fresh, shared TemporaryDirectory, never the repository, so
    a sample cannot accidentally import something from the working tree and
    so nothing it writes touches the repo.
    """
    samples = sorted(codegen_corpus_dir().glob("*.py"))
    if not samples:
        print(f"no *.py files found under {codegen_corpus_dir()}", file=sys.stderr)
        return 1

    failures = []
    with tempfile.TemporaryDirectory() as tmpdir:
        tmpdir_path = pathlib.Path(tmpdir)
        for sample in samples:
            lines = sample.read_text(encoding="utf-8").splitlines(keepends=True)
            try:
                expected_lines, header_count = parse_stdout_label(lines)
            except ValueError as exc:
                print(f"MALFORMED  {sample.name}: {exc}")
                failures.append(f"{sample}: {exc}")
                continue

            tmp_file = tmpdir_path / sample.name
            tmp_file.write_text("".join(strip_header(lines, header_count)), encoding="utf-8")

            status, detail = _check_codegen_label(sample.name, tmp_file, tmpdir, expected_lines)
            if status == "OK":
                print(f"OK         {sample.name}")
            else:
                print(f"{status:<10} {sample.name}")
                failures.append(f"{sample}: {detail}")

    if failures:
        print("\nFAILURES:", file=sys.stderr)
        for failure in failures:
            print(f"  - {failure}", file=sys.stderr)
        return 1

    print(f"\nAll {len(samples)} codegen sample(s) match their '# stdout:' label.")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    # Mutually exclusive: passing both used to silently run only
    # --generate-class-table and ignore --check-corpus entirely (argparse
    # just keeps the last-set store_true flags, it does not warn), which
    # would look like a corpus check ran when it never did.
    group = parser.add_mutually_exclusive_group()
    group.add_argument("--generate-class-table", action="store_true")
    group.add_argument("--generate-function-table", action="store_true")
    group.add_argument("--generate-object-member-table", action="store_true")
    group.add_argument(
        "--check-corpus",
        action="store_true",
        help="Run mypy --strict on every test_files/semantic sample and confirm its "
        "'# mypy:' header matches, run CPython on every sample carrying a "
        "'# cpython:' header to confirm that half too, AND run CPython on every "
        "test_files/codegen sample to confirm its '# stdout:' header (see "
        "--check-codegen). One command, every label in the repository. "
        "Developer-run only -- never part of ctest.",
    )
    group.add_argument(
        "--check-codegen",
        action="store_true",
        help="Run every test_files/codegen/*.py sample under real CPython and confirm "
        "its '# stdout:' header matches. This is the other half of the asymmetry "
        "codegen_corpus_test.cpp's own header comment names: ctest proves the emitted "
        "C++ program matches the RECORDED label, this proves the recorded label "
        "matches CPython. Developer-run only -- never part of ctest.",
    )
    args = parser.parse_args()
    if args.generate_class_table:
        sys.stdout.write(generate_class_table())
        return 0
    if args.generate_function_table:
        sys.stdout.write(generate_function_table())
        return 0
    if args.generate_object_member_table:
        sys.stdout.write(generate_object_member_table())
        return 0
    if args.check_codegen:
        return check_codegen_labels()
    if args.check_corpus:
        # Both arms run regardless of whether the first fails, so a single
        # invocation always reports on every label family rather than
        # stopping at the first broken one.
        semantic_result = check_corpus()
        print()
        codegen_result = check_codegen_labels()
        return 1 if (semantic_result != 0 or codegen_result != 0) else 0
    parser.print_help()
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
