# mypy: clean
# cpython: clean
# THE ROUND-3 DEFECT, pinned as a corpus sample so it re-derives against both
# real oracles rather than only against a hand-written expectation. Both arms
# of the inner `if` leave -- one breaks, one returns -- so `running = running
# + n + 1` is dead code. At 2997f6f the join had no edge left to keep, fell
# back to keeping both, and so read `n` at its declared `object`, producing
# `unsupported operand types for + ("int" and "object")` on a program both
# oracles accept. No merge state could have fixed that: the statement itself
# is unreachable, and mypy does not type-check unreachable code.
def total(xs: list[int], flag: bool) -> int:
    n: object = object()
    running: int = 0
    for x in xs:
        if flag:
            n = 7
            break
        else:
            return 0
        running = running + n + 1
    return running


print(total([1], True))
print(total([], False))
