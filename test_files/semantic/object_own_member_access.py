# mypy: clean
# cpython: clean
# `object`'s own real members (__class__, __repr__, __hash__, ...) are not
# modelled anywhere: a plain user class records no bases at all, so
# ClassTable::inherits_builtin_class's chain walk (the fix for a class
# reaching a SEEDED builtin row) never runs on it, and `object` is
# deliberately excluded from that walk even when a chain does reach it (see
# that function's own comment). So `p.__class__`, `p.__repr__()` and
# `p.__hash__()` were false TypeErrors on this exact program, and the
# identical false positive reached `object()` used directly. Driven so
# CPython actually executes the construction and every attribute access, not
# just defines the class.
class Plain:
    pass


p = Plain()
print(p.__class__)
print(p.__repr__())
print(p.__hash__() is not None)

o = object()
print(o.__class__)
