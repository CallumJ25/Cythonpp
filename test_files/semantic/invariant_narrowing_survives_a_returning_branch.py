# mypy: clean
# cpython: clean
class Bag:
    n: object = object()

    def pick(self, flag: bool) -> int:
        if flag:
            self.n = 7
        else:
            return 0
        return self.n + 1


print(Bag().pick(True))
print(Bag().pick(False))
