# mypy: clean
# cpython: clean
#
# A STATICALLY-DEAD ARM'S *INFERRED* BINDING does not fix the declared type.
# The sibling of `statically_dead_arm.py`, and the distinction is the whole
# point: that sample is about diagnostics raised INSIDE a dead arm, which a
# suppression can drop. This one is about a diagnostic raised on the LIVE
# line -- the dead arm bound the name first, so the live assignment was
# checked against the dead arm's type and reported. No suppression reaches
# that, because the statement it lands on is reachable.
#
# mypy takes the declared type from the LIVE arm only, and measured
# 2026-09-21 this holds at function, nested-function and method scope. It
# does NOT hold at module or class-body scope -- `reveal_type(x)` there is
# the DEAD arm's type and mypy reports -- so nothing in this sample is
# written at those scopes.
#
# Deliberately NOT here, because each is a program mypy REJECTS and a
# `# mypy: clean` sample carrying one fails the harness by construction:
#   - the same shape at module or class-body scope;
#   - an ANNOTATED dead binding (`x: str = "s"`), a bare `x: str`, or a
#     nested `def x()` in a dead arm -- the rule is about INFERRED types, and
#     all three commit at every scope;
#   - `if "":` / `if 0.0:` / a real `bool` condition, which are outside the
#     fold set so the arm stays LIVE;
#   - a read ABOVE the live binding (`if False: x = "s"` / `print(x)` /
#     `x = 1`), which is mypy `Cannot determine type of "x"` and a CPython
#     UnboundLocalError -- both oracles reject it and so must this compiler.
#
# Every function is driven below so CPython actually executes every path.


def headline() -> int:
    if False:
        x = "s"
    else:
        x = 1
    return x


def flat() -> int:
    if False:
        x = "s"
    x = 2
    return x


def every_folded_spelling() -> int:
    if 0:
        a = "s"
    a = 3
    if None:
        b = "s"
    b = a
    if not True:
        c = "s"
    c = b
    return c


def dead_else_of_a_true_guard() -> int:
    if True:
        x = 4
    else:
        x = "s"
    return x


def dead_elif_arm(c: bool) -> int:
    if c:
        x = 5
    elif False:
        x = "s"
    else:
        x = 6
    return x


def dead_while_body() -> int:
    while False:
        x = "s"
    x = 7
    return x


def dead_else_of_a_true_loop() -> int:
    while True:
        x = 8
        break
    else:
        x = "s"
    return x


def dead_nested_inside_a_live_arm(c: bool) -> int:
    if c:
        if False:
            x = "s"
        x = 9
    else:
        x = 10
    return x


def a_live_arm_nested_inside_a_dead_one() -> int:
    if False:
        if True:
            x = "s"
        else:
            pass
    x = 11
    return x


def dead_arm_inside_a_loop_body() -> int:
    total = 0
    for i in [1, 2]:
        if False:
            step = "s"
        step = i
        total = total + step
    return total


def a_for_target_in_a_dead_arm() -> int:
    if False:
        for x in ["s"]:
            pass
    x = 12
    return x


def a_tuple_unpack_in_a_dead_arm() -> int:
    if False:
        x, y = "s", "t"
    x = 13
    return x


def across_the_numeric_tower() -> float:
    if False:
        n = 1
    n = 14.5
    if False:
        m = True
    m = 15
    return n + m


def downstream_uses() -> int:
    # The narrowing JOIN matters as much as the declared type: without
    # dropping the dead arm's edge these read as `str | int` and draw a
    # NotImplementedError on the operand, the return and the argument alike.
    if False:
        x = "s"
    else:
        x = 1
    return double(x + 16)


def double(n: int) -> int:
    return n + n


def in_a_nested_def() -> int:
    def inner() -> int:
        if False:
            x = "s"
        x = 17
        return x

    return inner()


class Holder:
    def in_a_method(self) -> int:
        if False:
            x = "s"
        x = 18
        return x


print(headline())
print(flat())
print(every_folded_spelling())
print(dead_else_of_a_true_guard())
print(dead_elif_arm(True))
print(dead_elif_arm(False))
print(dead_while_body())
print(dead_else_of_a_true_loop())
print(dead_nested_inside_a_live_arm(True))
print(dead_nested_inside_a_live_arm(False))
print(a_live_arm_nested_inside_a_dead_one())
print(dead_arm_inside_a_loop_body())
print(a_for_target_in_a_dead_arm())
print(a_tuple_unpack_in_a_dead_arm())
print(across_the_numeric_tower())
print(downstream_uses())
print(in_a_nested_def())
print(Holder().in_a_method())
