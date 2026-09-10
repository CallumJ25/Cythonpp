# mypy: error Incompatible default for argument "a" (default has type "object", argument has type "int") [assignment]
# cythonpp: TypeError:11:5 incompatible default for argument "a" (default has type "object", argument has type "int")
# cpython: clean
# The default is checked against the DECLARED type of x (object), not the
# narrowed type (int) it carries on the line above -- mypy rejects, but
# CPython evaluates the default once at def-time and prints the narrowed
# value, so this pins the union rule's mypy-rejects/CPython-accepts half.
def f() -> None:
    x: object = object()
    x = 7
    def inner(a: int = x) -> None:
        print(a)
    inner()
f()
