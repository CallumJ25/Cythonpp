# mypy: clean
# cpython: clean
# A first assignment of None is a mypy PARTIAL type, not a declaration that
# the name holds None: the declared type comes from the assignment that
# RESOLVES the partial, and is that resolver's type UNIONED WITH None. So
# every read and every later assignment below is accepted by both oracles,
# including re-assigning None after the partial has been resolved -- which is
# the observable proof that the declared type absorbed None rather than
# becoming the bare resolver type.
value = None
value = 7
print(value)
value = None
print(value)

label = None
print(label)
label = "resolved"
print(label)

optional: int | None = 3
carried = None
carried = optional
print(carried)


def resolved_in_a_body(flag: bool) -> None:
    inner = None
    if flag:
        inner = 1
    print(inner)


resolved_in_a_body(True)
resolved_in_a_body(False)
