# mypy: clean
# cpython: clean
# mypy's CONDITIONAL-FUNCTION-DEFINITION allowance is NOT scope-limited, and
# cythonpp's function-scope binding site had no conditionality test at all --
# so every shape below drew a false `TypeError: name "g" already defined on
# line N` on a program `mypy --strict` and CPython both accept. Four distinct
# conditional forms are exercised: both arms of an if/else, a flat def then a
# conditional one, two defs in the same loop body, and a def nested two
# blocks deep.
#
# NO `# cythonpp:` LINE, deliberately: semantic_corpus_test.cpp reads that as
# "cythonpp must report ZERO diagnostics", which is the hardest pin available
# and the one this defect could not have survived.
def both_arms(c: bool) -> int:
    if c:
        def g() -> int:
            return 0

    else:
        def g() -> int:
            return 1

    return g()


def flat_then_conditional(c: bool) -> int:
    def g() -> int:
        return 0

    if c:
        def g() -> int:
            return 1

    return g()


def twice_in_one_loop(xs: list[int]) -> int:
    total: int = 0
    for x in xs:
        def g() -> int:
            return 0

        def g() -> int:
            return 1

        total = total + g() + x
    return total


def nested_two_blocks_deep(c: bool) -> int:
    def g() -> int:
        return 0

    if c:
        if not c:
            def g() -> int:
                return 1

    return g()


print(both_arms(True))
print(both_arms(False))
print(flat_then_conditional(True))
print(twice_in_one_loop([1, 2]))
print(nested_two_blocks_deep(True))
