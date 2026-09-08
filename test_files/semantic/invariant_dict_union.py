# mypy: clean
a: dict[str, int] = {}
b: dict[int, int] = {}
c = a | b
print(c)
