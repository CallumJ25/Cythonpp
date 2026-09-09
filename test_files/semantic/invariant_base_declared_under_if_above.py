# mypy: clean
FLAG = True

if FLAG:
    class Parent:
        def hello(self) -> int:
            return 1


class Child(Parent):
    pass


print(Child().hello())
