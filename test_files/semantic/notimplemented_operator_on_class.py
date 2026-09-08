# mypy: clean
# cythonpp: NotImplementedError:13:14 operators on user-defined class instances are not supported
class Point:
    def __init__(self, x: int, y: int) -> None:
        self.x = x
        self.y = y

    def __add__(self, other: Point) -> Point:
        return Point(self.x + other.x, self.y + other.y)


def add_points(a: Point, b: Point) -> Point:
    result = a + b
    return result
