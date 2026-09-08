# mypy: clean
# cythonpp: NotImplementedError:6:18 iterating an instance of a user-defined class is not supported
class IntList(list[int]):
    def total(self) -> int:
        acc: int = 0
        for v in self:
            acc = acc + v
        return acc
