# mypy: clean
class Sub(int):
    pass


x: int = Sub()
print(x)
