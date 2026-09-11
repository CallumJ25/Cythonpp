# mypy: error All conditional function variants must have identical signatures
# cpython: clean
# cythonpp: TypeError:18:5 name "g" already defined on line 14
# The other half of the conditional-function-definition rule, and a false
# NEGATIVE before this round: mypy allows a conditional redefinition only when
# the signatures are IDENTICAL, and this pair disagrees. Measured verbatim
# (mypy 1.18.1): `All conditional function variants must have identical
# signatures  [misc]` with notes `def g() -> int` / `def g(a: int) -> str`.
# cythonpp keeps its own `already defined` wording -- a wording divergence,
# not a compliance one, since both tools reject the program.
FLAG = True

if FLAG:
    def g() -> int:
        return 0

else:
    def g(a: int) -> str:
        return "s"


print(g)
