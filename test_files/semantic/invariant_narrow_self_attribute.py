# mypy: clean
class Bag:
    n: object = object()

    def bump(self) -> int:
        self.n = 7
        return self.n + 1

    def relabel(self) -> str:
        self.n = "s"
        return self.n + "!"


print(Bag().bump(), Bag().relabel())
