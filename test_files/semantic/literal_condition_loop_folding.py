# mypy: clean
# cpython: clean
#
# Literal folding in the two LOOP consumers.
#
# `always_true_loop_header`: a non-`True` always-true loop condition. mypy folds
# an int literal by VALUE in any base, so `while 1:` never exits normally and
# the function always returns. cythonpp's `is_literal_true` matched only a
# BOOL_TRUE Constant, so this was a false `missing return statement`.
#
# The rest are the break-reachability consumer: a folded guard makes the `break`
# in the branch it EXCLUDES unreachable, so the loop's `else` is guaranteed to
# run. Both loop kinds and both guard polarities are covered, because polarity
# decides WHICH arm dies -- an always-TRUE guard leaves its own body LIVE, which
# is why `if True: break` is a CONTROL that must keep reporting and is
# deliberately NOT in this sample (a `# mypy: clean` sample drawing a TypeError
# fails the harness by construction).
#
# Every function is driven below so CPython actually executes both paths.


def always_true_loop_header(n: int) -> int:
    while 1:
        return n + 1


def folded_guard_kills_the_break(c: bool) -> int:
    while c:
        if True:
            return 1
        break
    else:
        return 3


def inverted_polarity(c: bool) -> int:
    while c:
        if False:
            break
        else:
            return 1
    else:
        return 3


def the_for_sibling(xs: list[int]) -> int:
    for x in xs:
        if True:
            return x
        break
    else:
        return 3


def nested_folded_guards(c: bool) -> int:
    while c:
        if True:
            if True:
                return 1
        break
    else:
        return 3


print(always_true_loop_header(5))
print(folded_guard_kills_the_break(True))
print(folded_guard_kills_the_break(False))
print(inverted_polarity(True))
print(inverted_polarity(False))
print(the_for_sibling([7]))
print(the_for_sibling([]))
print(nested_folded_guards(True))
print(nested_folded_guards(False))
