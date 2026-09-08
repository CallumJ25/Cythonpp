# mypy: error name-defined
# cythonpp: NameError:4:13 name 'z' is not defined
def f(x: int) -> int:
    y = x + z
    return y
