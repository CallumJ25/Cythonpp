# stdout:
# 2
# 6
# 5
# 11
# 8
# 1
# 3
def bare_true(n: int) -> int:
    if True:
        return n + 1
    return 0


def bare_one(n: int) -> int:
    if 1:
        return n * 2
    return 0


def in_an_elif(c: bool, n: int) -> int:
    if c:
        return n
    elif True:
        return n + 10
    return 0


def always_true_loop_header(n: int) -> int:
    while 1:
        return n - 1


def folded_guard_kills_the_break(c: bool) -> int:
    while c:
        if True:
            return 1
        break
    else:
        return 3


print(bare_true(1))
print(bare_one(3))
print(in_an_elif(True, 5))
print(in_an_elif(False, 1))
print(always_true_loop_header(9))
print(folded_guard_kills_the_break(True))
print(folded_guard_kills_the_break(False))
