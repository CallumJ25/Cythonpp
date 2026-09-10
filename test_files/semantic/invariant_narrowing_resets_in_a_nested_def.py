# mypy: error Unsupported operand types for + ("object" and "int") [operator]
# cythonpp: TypeError:10:19 unsupported operand types for + ("object" and "int")
class Bag:
    n: object = object()

    def m(self) -> None:
        self.n = 7

        def inner() -> None:
            print(self.n + 1)

        inner()
