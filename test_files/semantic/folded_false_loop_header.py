# mypy: clean
# cpython: clean
#
# A loop header that folds FALSE never executes its body, so a `break` written
# there cannot run and the loop's `else` is guaranteed. mypy folds `False` and
# a bare zero int literal (its own `is_false_literal`, checker.py:8255), so
# each function below always returns -- where a purely syntactic scan finds the
# `break` textually and reports a false `missing return statement`.
#
# `after_a_returning_else` covers the second, distinct consumer: the folded
# loop's returning `else` starts an unreachable region, so the incompatible
# assignment below it is never type-checked. mypy prunes it; CPython never
# executes it.
#
# The CONTROLS are deliberately absent: `while False: break` with no `else`,
# and a `break` in the folded loop's own `else` arm (which targets the
# ENCLOSING loop and genuinely DOES run), are both programs mypy REJECTS, so a
# `# mypy: clean` sample carrying them would fail the harness by construction.
# They live in type_checker_test.cpp instead.


def bare_false() -> int:
    while False:
        break
    else:
        return 1


def bare_zero() -> int:
    while 0:
        break
    else:
        return 2


def body_returns_but_never_runs() -> int:
    while False:
        return 3
    else:
        return 4


def after_a_returning_else() -> int:
    while False:
        break
    else:
        return 5
    x: int = "never checked"
    print(x)


def nested_in_a_real_loop(c: bool) -> int:
    while c:
        while False:
            break
        return 6
    else:
        return 7


print(bare_false())
print(bare_zero())
print(body_returns_but_never_runs())
print(after_a_returning_else())
print(nested_in_a_real_loop(True))
print(nested_in_a_real_loop(False))
