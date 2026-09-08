# mypy: error operator
# cythonpp: TypeError:4:9 unsupported operand types for + ("int" and "str")
def combine(x: int) -> None:
    y = x + "s"
    print(y)
