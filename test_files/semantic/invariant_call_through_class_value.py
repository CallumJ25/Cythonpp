# mypy: clean
class Widget:
    def __init__(self, n: int) -> None:
        self.n = n


w = Widget
print(w(1).n)
