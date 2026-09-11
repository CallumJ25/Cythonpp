# mypy: error "Bag" has no attribute "q"  [attr-defined]
# cpython: clean
# cythonpp: TypeError:26:13 "Bag" has no attribute "q"
# cythonpp: TypeError:31:16 "Bag" has no attribute "q"
# Only a METHOD's own `self` declares an instance attribute. A nested
# `def inner(self: Bag)` inside a Bag method binds `self` to exactly
# Class("Bag"), so a guard that resolved `self` by TYPE alone passed and
# `self.q = 1` there declared "q" on Bag -- making the `read` below come out
# clean. TypeChecker::self_attribute_receiver_type now also requires the
# binding to be a method's own first parameter (Binding::method_self).
#
# mypy reports THREE errors here: attr-defined at the store, attr-defined at
# the read, and `Returning Any from function declared to return "int"` on the
# read's own return. This checker matches the two attr-defined ones and not
# the third -- a missed error, the safe direction, since the attribute
# expression types as Unknown once it has been reported and Unknown is
# absorbing everywhere in this checker.
#
# CPython is CLEAN, and that is not a contradiction: the module body only
# DEFINES the class, so neither `m` nor `read` ever runs and no AttributeError
# is ever raised. mypy alone rejects this program, which the union rule
# already covers -- if either oracle rejects it, cythonpp must not accept it.
class Bag:
    def m(self) -> None:
        def inner(self: Bag) -> None:
            self.q = 1

        inner(self)

    def read(self) -> int:
        return self.q
