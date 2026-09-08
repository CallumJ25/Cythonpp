# mypy: clean
a: list[int] = [1, 2]
b: list[str] = ["x", "y"]
c = a + b
print(c)
