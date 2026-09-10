# mypy: error Name "nope_not_defined" is not defined  [name-defined]
# cpython: clean
# cythonpp: NameError:13:11 name 'nope_not_defined' is not defined
# THE DISCRIMINATOR between "unreachable code is not type-checked" and
# "unreachable code is not visited at all". mypy reports a NAME error here --
# its semantic analyzer runs everywhere -- while a bad attribute, a bad call
# arity and a bad argument type in this same position are all silent. CPython
# accepts the program (the `return` runs first), so mypy is the oracle that
# rejects it, and the union rule says cythonpp must not go silent. A checker
# that stopped walking at the `return` would.
def f() -> int:
    return 0
    print(nope_not_defined)


print(f())
