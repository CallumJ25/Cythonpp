# mypy: clean
# cpython: clean
# A loop-carried accumulator, guarded by a flag so the read only executes
# once a PRIOR iteration has already run the write -- an ordinary,
# idiomatic Python pattern. Widening pre_bind_function_body to recurse into
# For/While bodies (closing a real union-rule violation elsewhere in this
# corpus) made the line-based ordering check fire across this loop's own
# BACK-EDGE, where mypy's flow-sensitive check does not: `print(total)` sits
# on an earlier LINE than `total = x`, but on the second and later
# iterations it executes AFTER a prior iteration's assignment already ran.
# Driven so CPython actually executes both branches of the loop.
total = 0


def run(xs: list[int]) -> None:
    started = False
    for x in xs:
        if started:
            print(total)
        total = x
        started = True


run([1, 2, 3])
