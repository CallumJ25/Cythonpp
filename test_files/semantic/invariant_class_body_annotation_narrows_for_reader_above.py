# mypy: clean
class Base:
    v: object


class Child(Base):
    def use(self) -> int:
        return self.v + 1

    v: int


print(Child().use())
