# mypy: clean
# cythonpp: NotImplementedError:5:5 methods on builtin types are not supported
def f() -> None:
    xs = [1, 2]
    xs.append(3)
