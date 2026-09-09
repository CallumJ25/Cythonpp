# mypy: clean
# cpython: error NameError: name 'Parent' is not defined
# cythonpp: NameError:8:13 name 'Parent' is not defined
# A program compiles here only when BOTH oracles accept it. mypy --strict is
# silent -- it resolves a forward-declared base fully and is order-insensitive
# -- but CPython raises at import, from the `class Child(Parent)` statement
# itself, so compiling this would turn a diagnostic gap into a wrong-code bug.
class Child(Parent):
    pass


class Parent:
    pass
