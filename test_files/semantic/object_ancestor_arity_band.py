# mypy: clean
# cpython: clean
# `object` is a seeded row with its own bounded (0, 0) constructor arity
# band -- not merely the absence of one an ordinary user class has -- so a
# whole-chain, first-branch-wins arity lookup can hand that band to an
# unrelated ancestor reached only by walking through an intermediate class.
# `M` contributes no band of its own; its `object` base must not answer on
# behalf of `C`'s real (unbounded) BaseException constructor. Driven so
# CPython actually executes the construction, not just defines the classes.
class M(object):
    pass


class C(M, Exception):
    pass


x = C("boom")
print(x)
