# mypy: clean
# cpython: clean
# The generated class table records each builtin's constructor arity band, so
# a builtin with no __init__ no longer defaults to zero-arg. All five of
# these are mypy --strict clean and all five run under CPython.
memoryview(b"ab")
property(None)
slice(1)
staticmethod(len)
type(1)
