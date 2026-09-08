# mypy: clean
def log(msg: str, level: int = 1) -> None:
    print(msg)
    print(level)


class Greeter:
    def __init__(self, name: str = "world") -> None:
        self.name = name

    def greet(self, punct: str = "!") -> str:
        return self.name + punct


log("start")
log("start", 2)
g = Greeter()
print(g.greet())
print(Greeter("ann").greet("?"))
