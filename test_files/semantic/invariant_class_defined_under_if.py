# mypy: clean
FLAG = True

if FLAG:
    class Bag:
        def __init__(self) -> None:
            self.n = 0

        def bump(self) -> int:
            self.n = self.n + 1
            return self.n


def use() -> int:
    b = Bag()
    return b.bump()


print(use())
