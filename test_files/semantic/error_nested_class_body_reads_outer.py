# mypy: error name-defined
# cythonpp: NameError:7:18 name 'x' is not defined
class C1:
    x: int = 1

    class C2:
        y: int = x
