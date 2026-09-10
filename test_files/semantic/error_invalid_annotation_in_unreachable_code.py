# mypy: error valid-type
# cpython: clean
# cythonpp: TypeError:11:8 not a valid type annotation
# Whether an annotation is a well-formed TYPE at all is semantic-analyzer
# output too (mypy [valid-type] and [type-arg]), so it is reported in
# unreachable code exactly like a redefinition is. This class was not among
# the five shapes the round-3 review found; it came out of auditing what each
# "TypeError" report site actually asserts.
def f() -> None:
    return
    x: 5 = 1


print("module ran")
