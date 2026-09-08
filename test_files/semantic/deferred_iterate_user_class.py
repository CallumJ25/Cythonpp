# mypy: clean
# cythonpp: NotImplementedError:17:10 iterating an instance of a user-defined class is not supported
class Counter:
    def __init__(self) -> None:
        self.n = 0

    def __next__(self) -> int:
        self.n = self.n + 1
        return self.n


class Bag:
    def __iter__(self) -> Counter:
        return Counter()


for v in Bag():
    print(v)
