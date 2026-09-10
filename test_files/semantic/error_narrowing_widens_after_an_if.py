# mypy: error Unsupported operand types for + ("object" and "int") [operator]
# cythonpp: TypeError:9:16 unsupported operand types for + ("object" and "int")
class Bag:
    n: object = object()

    def pick(self, flag: bool) -> int:
        if flag:
            self.n = 7
        return self.n + 1
