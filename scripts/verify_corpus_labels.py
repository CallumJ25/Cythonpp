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


def corpus_dir() -> pathlib.Path:
    # Relative to this script's own location, never a hardcoded absolute
    # path -- this file is tracked, so it must work on any machine's clone.
    return pathlib.Path(__file__).resolve().parent.parent / "test_files" / "semantic"


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


def parse_header(lines):
    """Returns (mypy_clean, header_line_count) for a sample's leading lines.

    Mirrors semantic_corpus_test.cpp's parse_labels: the header is line 1
    ("# mypy: clean" or "# mypy: error ...") plus every contiguous
    "# cythonpp: ..." line right after it. Raises ValueError for anything
    that doesn't match -- a label this cannot parse must fail loudly, not be
    silently skipped.

    Once the header ends (the first line that is neither), the REST of the
    lines are still scanned -- not ignored -- purely to catch a
    "# cythonpp:" line placed below the header by mistake. Silently dropping
    such a line would shrink the expected-diagnostics list without a trace,
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
    for line in lines[1:]:
        stripped = line.rstrip("\r\n")
        is_cythonpp_line = stripped.startswith("# cythonpp:")
        if in_header and is_cythonpp_line:
            header_count += 1
            continue
        if in_header:
            in_header = False  # First non-header line: the header is over.
        # Past the header now (possibly as of this very line). A
        # "# cythonpp:" line here is detached from the header block and
        # would otherwise vanish from the expected-diagnostics list without
        # a trace.
        if is_cythonpp_line:
            raise ValueError(
                "'# cythonpp:' line found below the header, detached from the leading "
                "'# mypy:'/'# cythonpp:' block -- move it up next to the other label "
                f"lines: {stripped}"
            )
    return mypy_clean, header_count


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


def check_corpus() -> int:
    """Runs mypy --strict on every test_files/semantic/*.py sample and
    reports whether each one's '# mypy:' header matches reality.

    Developer-run only -- NEVER invoked by ctest, which must stay hermetic
    (no Python, no network, no shelling out). This is the only thing in the
    project that actually confirms a '# mypy: clean' label is true rather
    than asserted from belief.

    mypy is run with its cwd set to a fresh TemporaryDirectory, so any
    .mypy_cache it writes never touches the repository at all.
    """
    samples = sorted(corpus_dir().glob("*.py"))
    if not samples:
        print(f"no *.py files found under {corpus_dir()}", file=sys.stderr)
        return 1

    failures = []
    with tempfile.TemporaryDirectory() as tmpdir:
        tmpdir_path = pathlib.Path(tmpdir)
        for sample in samples:
            lines = sample.read_text(encoding="utf-8").splitlines(keepends=True)
            try:
                mypy_clean, header_count = parse_header(lines)
            except ValueError as exc:
                print(f"MALFORMED  {sample.name}: {exc}")
                failures.append(f"{sample}: {exc}")
                continue

            tmp_file = tmpdir_path / sample.name
            tmp_file.write_text("".join(strip_header(lines, header_count)), encoding="utf-8")

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
            # (crash or fatal error -- bad flags, an internal exception,
            # etc). Only 0 and 1 are a real clean/error signal about the
            # SAMPLE. Treating >=2 as "error" (as `returncode == 0` alone
            # implicitly does) would let a crashed mypy invocation happily
            # "validate" every '# mypy: error' label without ever having
            # checked the code -- so it is scored as a hard failure instead,
            # distinct from a genuine label mismatch.
            if result.returncode >= 2:
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
                print(f"OK         {sample.name}  (label: {label})")
            else:
                print(f"MISMATCH   {sample.name}  (label: {label}, mypy says: {actual})")
                failures.append(
                    f"{sample}: labelled '# mypy: {label}' but mypy --strict says "
                    f"'{actual}':\n{result.stdout}{result.stderr}"
                )

    if failures:
        print("\nFAILURES:", file=sys.stderr)
        for failure in failures:
            print(f"  - {failure}", file=sys.stderr)
        return 1

    print(f"\nAll {len(samples)} sample(s) match their '# mypy:' header.")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    # Mutually exclusive: passing both used to silently run only
    # --generate-class-table and ignore --check-corpus entirely (argparse
    # just keeps the last-set store_true flags, it does not warn), which
    # would look like a corpus check ran when it never did.
    group = parser.add_mutually_exclusive_group()
    group.add_argument("--generate-class-table", action="store_true")
    group.add_argument(
        "--check-corpus",
        action="store_true",
        help="Run mypy --strict on every test_files/semantic sample and confirm its "
        "'# mypy:' header matches. Developer-run only -- never part of ctest.",
    )
    args = parser.parse_args()
    if args.generate_class_table:
        sys.stdout.write(generate_class_table())
        return 0
    if args.check_corpus:
        return check_corpus()
    parser.print_help()
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
