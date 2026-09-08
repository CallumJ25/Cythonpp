# mypy: clean
class Bag:
    def __init__(self, ys: list[int]) -> None:
        self.ys: list[int] = ys

    def total(self) -> int:
        return sum(self.ys)


print(Bag([1, 2]).total())
