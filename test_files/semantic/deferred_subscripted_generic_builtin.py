# mypy: clean
# cythonpp: NotImplementedError:4:8 generic builtin type 'zip' is not supported
def f() -> None:
    x: zip[int]
    print(x)
