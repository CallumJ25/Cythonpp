# mypy: clean
# cythonpp: NotImplementedError:5:9 repeating a tuple by an integer is not supported
def f() -> None:
    t = (1, 2)
    r = t * 2
    print(r)
