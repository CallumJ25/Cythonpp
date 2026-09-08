# mypy: clean
# cythonpp: NotImplementedError:4:9 operations on a union-typed value require narrowing, which is not supported
def f(x: int | float) -> None:
    y = x + 1
    print(y)
