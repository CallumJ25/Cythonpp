# mypy: clean
# cpython: clean
# A `while c:` header narrows `c` truthy for the whole loop body, so a guard
# on that same name is statically decided and a `break` in the branch the
# guard EXCLUDES is unreachable -- which makes the loop's `else` guaranteed to
# run, so each function below always returns. Both oracles accept and run all
# of these; this compiler used to report a false `missing return statement`.
def guard_true_break_after(flag: bool) -> int:
    while flag:
        if flag:
            return 1
        break
    else:
        return 3


def guard_false_break_inside(flag: bool) -> int:
    while flag:
        if not flag:
            break
        return 1
    else:
        return 3


def guard_true_break_in_else(flag: bool) -> int:
    while flag:
        if flag:
            return 1
        else:
            break
    else:
        return 3


def guard_nested_two_deep(flag: bool) -> int:
    while flag:
        if flag:
            if flag:
                return 1
        break
    else:
        return 3


print(guard_true_break_after(True))
print(guard_true_break_after(False))
print(guard_false_break_inside(True))
print(guard_false_break_inside(False))
print(guard_true_break_in_else(True))
print(guard_true_break_in_else(False))
print(guard_nested_two_deep(True))
print(guard_nested_two_deep(False))
