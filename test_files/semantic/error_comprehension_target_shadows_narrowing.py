# mypy: error Unsupported operand types for + ("str" and "int") [operator]
# cythonpp: TypeError:10:11 unsupported operand types for + ("str" and "int")
# `f` is never called, so this sample asserts nothing about CPython -- the
# invariant is that the comprehension's own `x` must NOT read the enclosing
# `x`'s narrowing to int, and both oracles agree the resulting `x` (bound
# over ["a", "b"]) is a str, so `x + 1` is a genuine error.
def f() -> None:
    x: object = object()
    x = 7
    ys = [x + 1 for x in ["a", "b"]]
    print(ys)
