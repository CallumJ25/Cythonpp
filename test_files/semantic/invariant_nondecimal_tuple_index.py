# mypy: clean
# cpython: clean
# A literal index selects ONE tuple element whenever the index's own type is a
# Literal, and neither a base prefix, nor a digit separator, nor bool being an
# int subtype, nor a chain of unary +/- changes that. Every line below is one
# of those spellings assigned to the ONE member's type, so declining any of
# them would show up here as a false TypeError naming the whole union.
#
# `~0` is the exception, and it is in this sample on purpose: `~` yields a
# plain int rather than a Literal, so mypy itself reveals the union there.
def use() -> str:
    t: tuple[int, str, float] = (1, "a", 2.0)
    hexed: str = t[0x1]
    upper: float = t[0X2]
    octal: float = t[0o2]
    binary: float = t[0b10]
    separated: float = t[0b1_0]
    flagged: str = t[True]
    zeroed: int = t[False]
    plussed: int = t[+0]
    folded: str = t[--1]
    from_end: float = t[+-1]
    inverted: int | str | float = t[~0]
    return (hexed + str(upper) + str(octal) + str(binary) + str(separated) + flagged +
            str(zeroed) + str(plussed) + folded + str(from_end) + str(inverted))


print(use())
