# mypy: clean
# cpython: clean
# The `break` half of invariant_narrowing_survives_a_continuing_branch.py,
# in a `while` body rather than a `for` body: `break` and `continue` are
# separate AST nodes reached through separate enclosing loops, so a fix that
# handled only one of either pair would leave the other reporting.
def total(k: int, flag: bool) -> int:
    n: object = object()
    running: int = 0
    while k > 0:
        k = k - 1
        if flag:
            n = 7
        else:
            break
        running = running + n + 1
    return running


print(total(2, True))
print(total(2, False))
