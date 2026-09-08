# mypy: clean
def g() -> None:
    print(1)


def maybe(x: int) -> None:
    if x < 0:
        return None
    print(x)


def forward(x: int) -> None:
    return g()
