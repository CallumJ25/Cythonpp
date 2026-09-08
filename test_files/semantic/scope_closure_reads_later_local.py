# mypy: clean
def outer() -> int:
    def inner() -> int:
        return value

    value = 5
    return inner()
