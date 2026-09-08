# mypy: error no-redef
# cythonpp: TypeError:4:1 name "x" already defined on line 3
x: int = 1
x: str = "s"
