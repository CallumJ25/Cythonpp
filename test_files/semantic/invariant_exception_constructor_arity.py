# mypy: clean
class MyError(Exception):
    pass


def raise_it(flag: int) -> None:
    if flag == 0:
        print(MyError())
    if flag == 1:
        print(MyError("boom"))
    print(MyError("boom", 42))


def others() -> None:
    print(ValueError("bad", 1, 2))
    print(OSError(2, "no such file"))
