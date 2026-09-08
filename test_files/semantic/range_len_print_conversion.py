# mypy: clean
def describe(n: int) -> str:
    result = 0
    for i in range(n):
        result = result + i
    print(len(str(n)))
    return str(float(result))
