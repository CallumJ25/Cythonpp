# mypy: clean
FLAG = True


def pick() -> int:
    return 1


if FLAG:
    def pick() -> int:
        return 2


print(pick())
