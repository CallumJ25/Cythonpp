# mypy: clean
# cpython: clean
# A method's first parameter is not required to be spelled `self`. mypy keys
# on which binding the receiver resolves to, not on the name, so this
# declares `q` on Bag exactly as `self.q = 1` would.
class Bag:
    def read(self) -> int:
        return self.q

    def m(this) -> None:
        this.q = 1


b = Bag()
b.m()
print(b.read())
