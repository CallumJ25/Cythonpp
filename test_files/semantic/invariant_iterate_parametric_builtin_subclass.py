# mypy: clean
class IntList(list[int]):
    def total(self) -> int:
        acc: int = 0
        for v in self:
            acc = acc + v
        return acc
