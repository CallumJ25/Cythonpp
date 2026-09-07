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

}} // namespace cythonpp::domain::semantic

#endif // CYTHONPP_DOMAIN_SEMANTIC_BUILTIN_CLASS_TABLE_H
"""


def class_names():
    return [
        name
        for name in sorted(dir(builtins))
        if isinstance(getattr(builtins, name), type) and not name.startswith("_")
    ]


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
    return HEADER.format(
        version=sys.version.split()[0],
        system=platform.system(),
        max_bases=MAX_BASES,
        rows="\n".join(rows),
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
