# stdout:
# -1
# 1
# 0
# 0
# 2
# -3
# -1
# 1
# False
# -7
# 7
flag: bool = True
off: bool = False
print(-flag)
print(+flag)
print(-off)
print(+off)
print(+flag + 1)
print(-flag * 3)
widened: float = -flag
print(widened)
count: int = +flag
print(count)
print(not flag)
print(-7)
print(+7)
