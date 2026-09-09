# mypy: clean
class Cache:
    def fetch(self) -> int:
        item = Item()
        return item.n


class Item:
    def __init__(self) -> None:
        self.n = 0


print(Cache().fetch())
