# mypy: error attr-defined
# cythonpp: TypeError:9:7 "Point" has no attribute "y"
class Point:
    def __init__(self, x: int) -> None:
        self.x = x


p = Point(1)
print(p.y)
