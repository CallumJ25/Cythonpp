# mypy: error call-arg
# cythonpp: TypeError:7:1 too few arguments for "f"
def f(x: int, y: int) -> int:
    return x + y


f(1)
