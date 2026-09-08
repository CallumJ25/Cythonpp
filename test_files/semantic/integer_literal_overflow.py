# mypy: clean
# cythonpp: OverflowError:4:9 integer literal is too large for a 64-bit integer
def f() -> None:
    x = 99999999999999999999
    print(x)
