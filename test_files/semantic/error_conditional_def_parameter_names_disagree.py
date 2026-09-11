# mypy: error All conditional function variants must have identical signatures
# cpython: clean
# cythonpp: TypeError:25:5 name "module_scope" already defined on line 21
# cythonpp: TypeError:34:9 name "g" already defined on line 30
# cythonpp: TypeError:46:9 name "g" already defined on line 42
# The other direction of the same root cause as
# invariant_conditional_def_union_order.py, and a false NEGATIVE before this
# round: mypy's "identical signatures" compares parameter NAMES, and
# Type::callable carries only {parameter types, return type, defaulted count}
# -- no names at all. So a pair differing ONLY in a parameter name compared
# equal and was silently accepted, which the union rule forbids because one
# oracle rejects the program. Measured verbatim (mypy 1.18.1) for each pair:
# `All conditional function variants must have identical signatures  [misc]`
# with notes naming the two spellings. CPython runs all three functions
# without complaint, so the `# cpython: clean` label above is honest and mypy
# is the oracle doing the rejecting. cythonpp keeps its own `already defined`
# wording -- a wording divergence, not a compliance one.
FLAG = True

if FLAG:
    def module_scope(a: int) -> int:
        return a

else:
    def module_scope(b: int) -> int:
        return b


def name_differs(c: bool) -> int:
    def g(a: int) -> int:
        return a

    if c:
        def g(b: int) -> int:
            return b

    return g(1)


def names_swapped(c: bool) -> str:
    if c:
        def g(a: int, b: str) -> str:
            return b

    else:
        def g(b: int, a: str) -> str:
            return a

    return g(1, "s")


print(module_scope(1))
print(name_differs(True))
print(names_swapped(True))
