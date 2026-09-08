# mypy: error return
# cythonpp: TypeError:3:1 missing return statement
def f(x: int) -> int:
    if x > 0:
        return x
