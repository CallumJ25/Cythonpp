# mypy: clean
# cythonpp: NotImplementedError:3:5 calls to builtin 'round' with these argument types are not supported
b = round(1.5, 2)
print(b)
