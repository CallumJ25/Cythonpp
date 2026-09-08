# mypy: clean
# cythonpp: NotImplementedError:6:13 calls to builtin 'zip' are not supported
def f() -> None:
    xs = [1, 2, 3]
    ys = [4, 5, 6]
    pairs = zip(xs, ys)
    print(pairs)
