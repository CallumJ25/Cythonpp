# mypy: clean
class Bag:
    n: object

    def set(self) -> None:
        self.n: int = 7

    def relabel(self) -> None:
        self.n = "s"


print(Bag().n)
