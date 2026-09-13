# mypy: clean
# cpython: clean
# cythonpp: NotImplementedError:18:9 methods on builtin types are not supported
# `super()`'s type is the seeded builtin class row `super` (a name/bases pair
# with no MEMBERS at all, exactly like the other 96 rows in
# builtin_class_table.h). `super().__init__()` is the single most common
# idiom this ever affected -- a subclass `__init__` chaining into its base's
# -- and was a false attr-defined TypeError before ClassTable's per-Entry
# `is_seeded_builtin` flag existed to defer it instead. Driven so CPython
# actually executes the construction, not just defines the classes.
class Base:
    def __init__(self) -> None:
        self.v = 1


class Child(Base):
    def __init__(self) -> None:
        super().__init__()
        self.w = 2


c = Child()
print(c.v)
print(c.w)
