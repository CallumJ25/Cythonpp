# mypy: error arg-type
# cythonpp: TypeError:7:3 argument 1 to "f" has incompatible type "str"; expected "int"
def f(x: int) -> None:
    print(x)


f("s")
