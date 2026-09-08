# mypy: error assignment
# cythonpp: TypeError:8:5 incompatible types in assignment (expression has type "object", variable has type "int")
class Base:
    v: int


class Child(Base):
    v: object
