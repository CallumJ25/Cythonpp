# mypy: clean
class Base:
    v: object


class Child(Base):
    v: int

    def use(self) -> int:
        return self.v + 1


print(Child().use())
