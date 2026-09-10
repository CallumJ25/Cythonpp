# mypy: clean
# cpython: clean
# THE SECOND HALF OF THE ROUND-3 FIX, and the one that shows why suppressing
# the diagnostic is not enough on its own. `n = "s"` is dead code, so its own
# TypeError goes unreported -- but if it still RECORDED a narrowing, the
# loop's end-of-body snapshot would carry `n` as `str`, the loop join would
# yield `int | str`, and the REACHABLE `k: int = n` would draw `incompatible
# types in assignment (expression has type "int | str", variable has type
# "int")` from outside the suppressed region. That is exactly what 2997f6f
# did. The narrowing state is now restored to its value as of the terminator
# before the suite walk returns, which is where the suite genuinely ends.
def m(flag: bool) -> int:
    n: object = object()
    n = 7
    while flag:
        if flag:
            break
        else:
            break
        n = "s"
    k: int = n
    return k


print(m(False))
print(m(True))
