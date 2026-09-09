# mypy: clean
# cythonpp: NotImplementedError:17:11 calls to 'Counted', which inherits an overloaded builtin constructor, are not supported
class Counted(int):
    pass


class Mixin:
    def __init__(self, a: str) -> None:
        print(a)


class Ordered(Mixin, int):
    pass


def build() -> None:
    print(Counted(3))
    print(Counted())
    print(Ordered("s"))
