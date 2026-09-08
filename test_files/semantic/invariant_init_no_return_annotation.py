# mypy: clean
class Point:
    def __init__(self, a: int):
        self.a = a


p = Point(3)
print(p.a)
