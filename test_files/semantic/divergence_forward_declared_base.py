# mypy: clean
# cythonpp: NameError:7:13 name 'Parent' is not defined
# CPython raises `NameError: name 'Parent' is not defined` at import from the
# `class Child(Parent)` statement itself, so reporting here is correct for a
# compiler that emits code meant to run. mypy resolves the base fully and is
# order-insensitive; that disagreement is the point of this sample.
class Child(Parent):
    pass


class Parent:
    pass
