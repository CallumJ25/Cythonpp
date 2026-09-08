# mypy: clean
class Counter:
    def __init__(self, start: int) -> None:
        self.value = start

    def bump(self) -> int:
        self.value = self.value + 1
        return self.value


def classify(n: int) -> str:
    if n < 0:
        return "negative"
    elif n == 0:
        return "zero"
    else:
        return "positive"


total: int = 0
limit: int = 5
i: int = 0
while i < limit:
    if i == 2:
        i = i + 1
        continue
    if i == 4:
        break
    total = total + i
    i = i + 1

values = [1, 2, 3, 4, 5]
for v in values:
    if v == 3:
        pass
    print(classify(v))

squares = [v * v for v in values]
counter = Counter(0)
counter.bump()
print(total)
print(squares)
