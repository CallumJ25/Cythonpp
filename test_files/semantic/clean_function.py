# mypy: clean
def add(x: int, y: int) -> int:
    return x + y


total: int = add(2, 3)
