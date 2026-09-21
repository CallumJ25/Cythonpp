# stdout:
# 2.5
# 2.5
# 7
# 2.5
# 3.25
# 4.5
# 5.5
# 6.5
# unbound-but-declared
def int_dead_float_live() -> float:
    if False:
        x = 1
    else:
        x = 2.5
    return x


def bool_dead_float_live() -> float:
    if False:
        x = True
    else:
        x = 2.5
    return x


def bool_dead_int_live() -> int:
    if False:
        x = True
    else:
        x = 7
    return x


def folded_true_body_stays_live() -> float:
    if True:
        x = 2.5
    else:
        x = 1
    return x


def flat_dead_arm_then_live_assignment() -> float:
    if False:
        x = 1
    x = 3.25
    return x


def dead_while_body_then_live_assignment() -> float:
    while False:
        x = 1
    x = 4.5
    return x


def nested_dead_arm_inside_a_live_one(c: bool) -> float:
    if c:
        if False:
            x = 1
        else:
            x = 5.5
    else:
        x = 6.5
    return x


def bound_only_in_a_dead_arm() -> str:
    if False:
        x = 1
    return "unbound-but-declared"


print(int_dead_float_live())
print(bool_dead_float_live())
print(bool_dead_int_live())
print(folded_true_body_stays_live())
print(flat_dead_arm_then_live_assignment())
print(dead_while_body_then_live_assignment())
print(nested_dead_arm_inside_a_live_one(True))
print(nested_dead_arm_inside_a_live_one(False))
print(bound_only_in_a_dead_arm())
