# mypy: clean
class Bag:
    n: object = object()

    def total(self, xs: list[int]) -> int:
        self.n = 7
        for x in xs:
            print(self.n + x)
        return self.n + 1


print(Bag().total([1, 2, 3]))
