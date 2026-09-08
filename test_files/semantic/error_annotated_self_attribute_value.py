# mypy: error list-item
# cythonpp: TypeError:5:31 list item 0 has incompatible type "str"; expected "int"
class Bag:
    def __init__(self) -> None:
        self.ys: list[int] = ["s"]
