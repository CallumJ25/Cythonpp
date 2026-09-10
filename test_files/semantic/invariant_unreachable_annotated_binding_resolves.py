# mypy: clean
# cpython: clean
# UNREACHABLE CODE STILL BINDS NAMES. `x` is bound ONLY after the `return`,
# and read from statically reachable code -- and both oracles accept the
# program, because `f` is never called so the read never executes. mypy's
# semantic analyzer runs over unreachable code even though its type checker
# does not, so a checker that stopped WALKING at the terminator would invent
# `NameError: name 'x' is not defined` here.
#
# The ANNOTATION is load-bearing: mypy reports `Cannot determine type of "x"
# [has-type]` for an unannotated `x = 1` in the same position, so only the
# annotated form is a clean program.
def f(c: bool) -> None:
    if c:
        return
        x: int = 1
    print(x)


print("module ran")
