# mypy: clean
# cythonpp: NotImplementedError:8:7 calling an instance of a user-defined class is not supported
class Widget:
    pass


w = Widget
print(w())
