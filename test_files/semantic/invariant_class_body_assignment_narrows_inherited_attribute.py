# mypy: clean
class Base:
    v: object


class Child(Base):
    v = 1

    def use(self) -> int:
        return self.v + 1

    def bump(self) -> None:
        self.v = 7


print(Child().use())
