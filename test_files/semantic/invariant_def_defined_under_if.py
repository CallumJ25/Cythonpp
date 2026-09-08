# mypy: clean
FLAG = True

if FLAG:
    def pick(n: int) -> int:
        return n + 1


def use() -> int:
    return pick(1)


print(use())
