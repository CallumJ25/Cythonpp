# mypy: clean
FLAG = True

if FLAG:
    def pick() -> int:
        return 1

else:
    def pick() -> int:
        return 2


print(pick())
