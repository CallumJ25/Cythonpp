# mypy: clean
# cpython: clean
#
# A STATICALLY-DEAD ARM is not type-checked. mypy prunes the arm a folded
# condition excludes, so the deliberately incompatible assignment inside each
# dead arm below is never reported -- the same line this compiler already drew
# for an unreachable REGION (after a `return`), now drawn for a dead ARM.
#
# POLARITY decides WHICH arm, and it INVERTS between `if` and `while`:
#   if <false>     -> the BODY is dead        while <false> -> the BODY is dead
#   if <true>      -> the ELSE arm is dead    while <true>  -> the ELSE is dead
# The loop cases differ in their reason: a folded-false loop never enters its
# body (so its `else` RUNS), and a folded-true loop never completes normally
# (so its `else` never runs).
#
# Deliberately NOT here, because each is a program mypy REJECTS and a
# `# mypy: clean` sample carrying one fails the harness by construction:
# an unbound name or a redefinition in a dead arm (mypy reports both, and so
# must this compiler -- that is exactly the existing Suppressibility split),
# and the live-arm controls `if True:`'s own body, `while False:`'s `else`,
# and the unfoldable `if "":` / `if 0.0:` guards.
#
# Every function is driven below so CPython actually executes the live paths.


def dead_if_body() -> int:
    if False:
        bad: int = "not an int"
        print(bad)
    return 1


def dead_else_of_a_true_guard() -> int:
    if True:
        return 2
    else:
        bad: int = "not an int"
        print(bad)


def dead_while_body() -> int:
    while False:
        bad: int = "not an int"
        print(bad)
    return 3


def dead_else_of_a_true_loop() -> int:
    while True:
        break
    else:
        bad: int = "not an int"
        print(bad)
    return 4


def dead_under_not_true() -> int:
    if not True:
        bad: int = "not an int"
        print(bad)
    return 5


def dead_under_zero_and_none() -> int:
    if 0:
        bad: int = "not an int"
        print(bad)
    if None:
        worse: str = 7
        print(worse)
    return 6


def nested_and_elif_dead(c: bool) -> int:
    if False:
        if True:
            bad: int = "not an int"
            print(bad)
    if False:
        other: int = "nor this"
        print(other)
    elif c:
        return 7
    return 8


print(dead_if_body())
print(dead_else_of_a_true_guard())
print(dead_while_body())
print(dead_else_of_a_true_loop())
print(dead_under_not_true())
print(dead_under_zero_and_none())
print(nested_and_elif_dead(True))
print(nested_and_elif_dead(False))
