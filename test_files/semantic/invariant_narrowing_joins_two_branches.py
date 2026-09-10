# mypy: clean
class Bag:
    n: object = object()

    def pick(self, flag: bool) -> int:
        if flag:
            self.n = 7
        else:
            self.n = 8
        return self.n + 1


print(Bag().pick(True))
