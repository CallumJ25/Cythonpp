# mypy: error used-before-def
# cpython: error UnboundLocalError: cannot access local variable 'self' where it is not associated with a value
# cythonpp: NameError:13:13 name 'self' is used before definition
# A closure captures the enclosing method's own `self`, but a `for self in
# ...:` anywhere in that SAME closure's body rebinds "self" to a local of the
# closure itself -- Python decides which names are local to a scope
# statically, for the WHOLE scope, regardless of where in it the rebind sits
# -- so the earlier `self.q = 1` reads that not-yet-assigned local rather
# than the captured receiver, and both oracles reject it.
class Bag:
    def m(self) -> None:
        def inner() -> None:
            self.q = 1
            for self in [1, 2]:
                pass
        inner()


b = Bag()
b.m()
