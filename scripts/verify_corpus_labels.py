#!/usr/bin/env python3
"""Developer tool. Regenerates builtin_class_table.h and re-derives corpus labels.

NEVER run by ctest. The test suite must pass with no Python installed, so this
script's output is CHECKED IN and the tests read the checked-in file.

Usage:
    python scripts/verify_corpus_labels.py --generate-class-table
    python scripts/verify_corpus_labels.py --check-corpus     # added in Task 24
"""

import argparse
import builtins
import collections
import platform
import sys

MAX_BASES = 4

HEADER = """#ifndef CYTHONPP_DOMAIN_SEMANTIC_BUILTIN_CLASS_TABLE_H
#define CYTHONPP_DOMAIN_SEMANTIC_BUILTIN_CLASS_TABLE_H

#include <cstddef>

namespace cythonpp::domain::semantic {{

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
// Seven entries are GENERIC in typeshed -- zip, map, filter, enumerate,
// reversed, staticmethod, classmethod. Bare use (`x: zip`) is a mypy type-arg
// error while ours is clean, a recorded direction-(b) miss. But `x: zip[int]`
// is mypy-CLEAN, so AnnotationResolver must report NotImplementedError for a
// subscripted seeded class, never "'zip' is not subscriptable" (Task 9).

// Four is enough: the widest direct-base list in the extraction is 2. The
// generator raises if that ever stops being true.
struct BuiltinClass {{
    const char* name;
    const char* bases[{max_bases}];
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


def generate_class_table() -> str:
    names = class_names()
    known = set(names)
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
        rows.append(f'    {{"{name}", {{{rendered}}}}},')

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


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--generate-class-table", action="store_true")
    args = parser.parse_args()
    if args.generate_class_table:
        sys.stdout.write(generate_class_table())
        return 0
    parser.print_help()
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
