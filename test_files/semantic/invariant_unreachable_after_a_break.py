# mypy: clean
# cpython: clean
# The simplest possible shape of the same rule: a statement after a BARE
# `break`, with no nested `if` in the way. mypy stops type-checking a suite at
# the first statement that always leaves, so `running + "s"` is never checked
# -- while the identical statement one line earlier, or in a suite whose
# `break` is conditional, is (see error_narrowing_misses_a_break_edge.py and
# the `AReachableBreak...` controls in type_checker_test.cpp).
def total(xs: list[int]) -> int:
    running: int = 0
    for x in xs:
        running = running + x
        break
        running = running + "s"
    return running


print(total([1, 2]))
print(total([]))
