# mypy: error Unsupported operand types for + ("object" and "int") [operator]
# cythonpp: TypeError:7:12 unsupported operand types for + ("object" and "int")
def total(xs: list[int]) -> int:
    x: object = "s"
    for x in xs:
        print(x)
    return x + 1


print(total([1, 2]))
