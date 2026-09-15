# stdout:
# 3628800
# 1
def factorial(n: int) -> int:
    if n < 2:
        return 1
    return n * factorial(n - 1)


print(factorial(10))
print(factorial(0))
