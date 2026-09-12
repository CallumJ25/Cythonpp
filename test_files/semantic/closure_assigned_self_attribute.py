# mypy: clean
# cpython: clean
# mypy attributes a `self.x = ...` store to the method's own first-parameter
# binding through any number of capturing closures, so a reader ABOVE the
# closure still sees the attribute. Both oracles accept and RUN this.
class Bag:
    def read(self) -> int:
        return self.q

    def m(self) -> None:
        def inner() -> None:
            self.q = 1

        inner()


b = Bag()
b.m()
print(b.read())
