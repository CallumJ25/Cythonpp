# mypy: clean
class Base:
    def __init__(self) -> None:
        self.v: object = 1


class Child(Base):
    def m(self) -> None:
        self.v: int = 1

    def use(self) -> int:
        return self.v + 1


print(Child().use())
