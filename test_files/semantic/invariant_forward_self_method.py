# mypy: clean
class A:
    def a(self) -> None:
        self.b()

    def b(self) -> None:
        print("b")


A().a()
