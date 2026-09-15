# stdout:
# 1
# 3
# 5
# 7
# 16
def run(count: int) -> int:
    i: int = 0
    total: int = 0
    while i < count:
        i = i + 1
        if i % 2 == 0:
            continue
        if i > 7:
            break
        print(i)
        total = total + i
    return total


print(run(10))
