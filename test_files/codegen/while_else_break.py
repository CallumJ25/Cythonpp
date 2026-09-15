# stdout:
# 6
# -1
def find_first_multiple(limit: int, factor: int) -> int:
    i: int = 1
    while i < limit:
        if i % factor == 0:
            break
        i = i + 1
    else:
        i = -1
    return i


print(find_first_multiple(20, 6))
print(find_first_multiple(5, 100))
