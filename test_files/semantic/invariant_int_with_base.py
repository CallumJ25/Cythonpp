# mypy: clean
# cythonpp: NotImplementedError:3:5 calls to builtin 'int' with these argument types are not supported
c = int("ff", 16)
print(c)
