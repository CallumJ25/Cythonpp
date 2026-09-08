# mypy: clean
# cythonpp: NotImplementedError:3:5 calls to builtin 'list' with these argument types are not supported
a = list(range(3))
print(a)
