# mypy: clean
class IntList(list[int]):
    def first(self) -> int:
        return self[0]


def total() -> int:
    acc = 0
    for v in IntList():
        acc = acc + v
    return acc


widened: list[int] = IntList()
print(widened, total())
