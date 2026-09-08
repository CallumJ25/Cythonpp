# mypy: clean
a: set[int] = set()
b: set[str] = set()
c = a <= b
print(c)
