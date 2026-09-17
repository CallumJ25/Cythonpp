# mypy: clean
# cpython: clean
# A dict SUBSCRIPT STORE resolves a bare-empty-container partial, to
# dict[K, V] taken from the key and value expression types, and the FIRST
# store commits: a second store of the same shape is clean, and a later
# whole-dict assignment is checked against what the store already committed.
# The asymmetry is mypy's own and is measured both ways -- a LIST subscript
# store is NOT a resolver (`x = []` / `x[0] = 1` stays `Need type
# annotation` under mypy and an `IndexError` under CPython), so no list store
# appears here; that direction is a control and cannot be a clean sample.
# Driven so CPython actually executes every line.
stored = {}
stored["a"] = 1
print(stored)

multi = {}
multi["a"] = 1
multi["b"] = 2
print(multi)

committed = {}
committed["x"] = 5
committed = {"y": 6}
print(committed)

keyed_by_int = {}
keyed_by_int[1] = "one"
print(keyed_by_int)


def stored_in_a_function(flag: bool) -> None:
    local_map = {}
    local_map["z"] = 8
    print(local_map)

    if flag:
        in_a_block = {}
        in_a_block["b"] = 9
        print(in_a_block)


stored_in_a_function(True)
stored_in_a_function(False)
