# mypy: error call-overload
# cpython: error TypeError: int() takes at most 2 arguments (3 given)
# cythonpp: NotImplementedError:9:11 calls to 'Counted', which inherits an overloaded builtin constructor, are not supported
class Counted(int):
    pass


def build() -> None:
    print(Counted(1, 2, 3))


build()
