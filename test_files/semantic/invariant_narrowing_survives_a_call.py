# mypy: clean
class Bag:
    n: object = object()

    def reset(self) -> None:
        self.n = "reset"

    def bump(self) -> int:
        self.n = 7
        self.reset()
        return self.n + 1


print(Bag().bump())
