# mypy: error assignment
# cythonpp: TypeError:10:9 incompatible types in assignment (expression has type "object", variable has type "int")
class Base:
    def __init__(self) -> None:
        self.v: int = 1


class Child(Base):
    def m(self) -> None:
        self.v: object = 1
