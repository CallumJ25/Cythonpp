# mypy: clean
# cythonpp: NotImplementedError:5:23 tuple targets in comprehensions are not supported
def f() -> None:
    pairs = [(1, 2), (3, 4)]
    sums = [a + b for a, b in pairs]
    print(sums)
