# stdout:
# 1
# 2
# True
# False
# 3
def f(x: bool) -> int:
    return x


x: float = 1
print(x)
x = 2
print(x)
print(f(True))
print(f(False))
y: float = x + 1
print(y)
