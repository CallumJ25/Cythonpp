# mypy: clean
def parts() -> tuple[int, str]:
    return (1, "a")


def use() -> str:
    t = parts()
    count: int = t[0]
    label: str = t[1]
    last: str = t[-1]
    return label + last + str(count)


print(use())
