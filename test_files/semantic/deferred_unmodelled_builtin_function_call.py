# mypy: clean
# cythonpp: NotImplementedError:4:11 calls to builtin 'hash' are not supported
def key(x: int) -> None:
    print(hash(x))
