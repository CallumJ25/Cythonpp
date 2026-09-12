# mypy: clean
# cpython: clean
# A loop whose body has no reachable break always runs its else, so the
# else's return is guaranteed and the function cannot fall off the end.
# Both oracles accept and RUN this: mypy --strict is Success and CPython
# prints 1 then 3.
def f(xs: list[int]) -> int:
    for x in xs:
        print(x)
    else:
        return 3


print(f([1]))
