# mypy: clean
# cythonpp: NotImplementedError:5:9 tuple targets in for loops are not supported
def f() -> None:
    pairs = [(1, 2), (3, 4)]
    for a, b in pairs:
        print(a)
