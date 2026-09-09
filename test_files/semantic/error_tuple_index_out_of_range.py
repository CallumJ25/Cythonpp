# mypy: error Tuple index out of range [misc]
# cythonpp: TypeError:5:11 tuple index out of range
def use() -> None:
    t: tuple[int, str] = (1, "a")
    print(t[5])
