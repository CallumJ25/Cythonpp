# mypy: clean
# cpython: clean
#
# A statically-true literal guard decides a return path, with NO loop and no
# narrowing involved. mypy folds `True` and a bare nonzero int literal (its own
# `is_true_literal`, checker.py:8255), so an `if` whose guard folds true always
# returns exactly when its body does -- even with an EMPTY else, which is what
# `always_returns`' If arm otherwise requires.
#
# The last function is the second, distinct consumer: a folded-true `return`
# starts an unreachable region, so the incompatible assignment below it is
# never type-checked. mypy prunes it (confirmed with --warn-unreachable:
# `Statement is unreachable [unreachable]`); CPython never executes it.


def bare_true(n: int) -> int:
    if True:
        return n + 1
    return 0


def bare_one(n: int) -> int:
    if 1:
        return n * 2
    return 0


def with_empty_else(n: int) -> int:
    if True:
        return n - 1
    else:
        pass
    return 0


def in_an_elif(c: bool, n: int) -> int:
    if c:
        return n
    elif True:
        return n + 100
    return 0


def unreachable_region_is_not_checked(n: int) -> int:
    if True:
        return n
    x: int = "not an int"
    return x


print(bare_true(1))
print(bare_one(3))
print(with_empty_else(10))
print(in_an_elif(True, 5))
print(in_an_elif(False, 5))
print(unreachable_region_is_not_checked(7))
