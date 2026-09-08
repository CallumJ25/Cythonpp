# mypy: clean
class Bag:
    def total(self) -> int:
        return sum(self.ys)

    def __init__(self, ys: list[int]) -> None:
        self.ys: list[int] = ys


print(Bag([1, 2]).total())
