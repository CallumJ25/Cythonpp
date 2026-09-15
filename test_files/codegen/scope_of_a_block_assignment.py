# stdout:
# 2
# 1
# 2
# True
# 2
# 2.5
def classify(c: bool) -> int:
    if c:
        result: int = 1
    else:
        result = 2
    return result


def widen(x: int) -> int:
    return x


def valueless() -> float:
    total: float
    total = 1
    total = 2.5
    return total


limit: int = 1
if limit > 0:
    y: int = 2
else:
    y = 3
print(y)
print(classify(True))
print(classify(False))
print(widen(True))
counter: int = 0
while counter < 2:
    counter = counter + 1
print(counter)
print(valueless())
