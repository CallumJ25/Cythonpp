# mypy: clean
# cpython: clean
# A branch ending in `continue` never reaches the statement after the `if`,
# so its end-of-branch state is not an edge at that merge. Taking it anyway
# contributes the DECLARED `object` for a path the branch never assigned,
# widening the surviving branch's `int` back to `object` -- a false TypeError
# on the `+` below, on a program both oracles accept.
def total(xs: list[int], flag: bool) -> int:
    n: object = object()
    running: int = 0
    for _ in xs:
        if flag:
            n = 7
        else:
            continue
        running = running + n + 1
    return running


print(total([1, 2], True))
print(total([1, 2], False))
