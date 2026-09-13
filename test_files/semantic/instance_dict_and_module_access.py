# mypy: clean
# cpython: clean
# `__dict__` and `__module__` are per-class-instance MACHINERY (every class
# without `__slots__` gets an instance `__dict__`, and every class gets a
# `__module__`) -- a property of the class machinery, not of `object`'s own
# member set, which is why builtin_object_member_table.h's generated
# kObjectMembers deliberately excludes both. A plain user class records no
# bases at all, so neither of the seeded-builtin-row carve-outs ever runs on
# it, and both names were false TypeErrors on this exact program. `object()`
# used directly keeps reporting for both (CPython genuinely raises
# AttributeError there, uniquely among every OTHER class) -- that half is
# unit-tested (ExpressionTyper.AnInstanceMachineryMemberOnObjectItselfStaysATypeError)
# rather than added to this corpus, matching object_own_member_access.py's own
# precedent of covering only the CLEAN half here. Driven so CPython actually
# executes the construction and every attribute access.
class Plain:
    pass


p = Plain()
print(p.__dict__)
print(p.__module__)
