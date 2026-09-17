# mypy: clean
# cpython: clean
# A first assignment of a BARE EMPTY container is a mypy PARTIAL type, not a
# declaration and not an error: the declared type comes from the assignment
# that RESOLVES the partial, and is that resolver's type EXACTLY -- with no
# union, deliberately unlike the sibling `None` partial, whose declared type
# absorbs None. `Need type annotation for "x"` is mypy's verdict only when
# the first thing that touches the name after the seed is NOT a resolver, so
# every read below sits AFTER its own resolver. All four resolvable spellings
# appear -- the `[]` and `{}` displays and the `list()` and `dict()` calls --
# at module, function, nested-function and class-body scope, which are the
# four scopes the suppression scan is wired at. Driven so CPython actually
# executes every line.
numbers = []
numbers = [1, 2]
print(numbers)

pairs = {}
pairs = {1: 2}
print(pairs)

called = list()
called = [3]
print(called)

mapped = dict()
mapped = {"k": 4}
print(mapped)


class Registry:
    entries = {}
    entries = {"a": 1}


def resolved_in_a_function(flag: bool) -> None:
    local_list = []
    local_list = [7]
    print(local_list)

    if flag:
        nested_in_a_block = []
        nested_in_a_block = [9]
        print(nested_in_a_block)

    def resolved_in_a_nested_function() -> None:
        deep = dict()
        deep = {"d": 10}
        print(deep)

    resolved_in_a_nested_function()


print(Registry.entries)
resolved_in_a_function(True)
resolved_in_a_function(False)
