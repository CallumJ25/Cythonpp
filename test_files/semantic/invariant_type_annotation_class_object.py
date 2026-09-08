# mypy: clean
class Widget:
    pass


class Outer:
    class Inner:
        pass


x: type = Widget
y: type = Outer.Inner
print(x, y)
