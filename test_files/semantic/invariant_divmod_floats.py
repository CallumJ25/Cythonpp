# mypy: clean
# cythonpp: NotImplementedError:3:5 calls to builtin 'divmod' with these argument types are not supported
d = divmod(7.0, 2.0)
print(d)
