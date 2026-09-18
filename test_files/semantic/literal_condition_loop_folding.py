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
# The `not_*` functions were added 2026-09-18, when the fold learned to look
# through `not`: it INVERTS a decided operand and leaves an undecided one
# undecided, so nesting works and every existing exclusion composes for free
# (`not ""`, `not 0.0`, `not -1` and `not 0x1` all still fold NEITHER way).
# Before that, `not_kills_a_guarded_break` had to be written with a bare
# `False` because `not True` was a retained false positive.
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


def not_over_a_false_literal() -> int:
    while not False:
        return 8


def not_kills_a_guarded_break(c: bool) -> int:
    while c:
        if not True:
            break
        else:
            return 9
    else:
        return 10


def nested_not_inverts_twice(c: bool) -> int:
    while c:
        if not not True:
            return 11
        break
    else:
        return 12


print(always_true_loop_header(5))
print(folded_guard_kills_the_break(True))
print(folded_guard_kills_the_break(False))
print(inverted_polarity(True))
print(inverted_polarity(False))
print(the_for_sibling([7]))
print(the_for_sibling([]))
print(nested_folded_guards(True))
print(nested_folded_guards(False))
print(not_over_a_false_literal())
print(not_kills_a_guarded_break(True))
print(not_kills_a_guarded_break(False))
print(nested_not_inverts_twice(True))
print(nested_not_inverts_twice(False))
