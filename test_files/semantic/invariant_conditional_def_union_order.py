# mypy: clean
# cpython: clean
# mypy's "All conditional function variants must have identical signatures"
# allowance is UNION-ORDER-INSENSITIVE, and implementing "identical" as
# Type::operator== -- which type.h documents as exact and order-SENSITIVE for
# unions -- made every shape below a false `TypeError: name "g" already
# defined on line N` on a program `mypy --strict` and CPython both accept.
# The module-scope pair was a regression introduced by the round that added
# the equality test; the function-scope pairs predate it.
#
# Six positions for the reordered union are exercised, because
# order-insensitivity at the top level alone leaves every nested one broken:
# a parameter, the RETURN type, inside a `list` argument, inside a `dict`'s
# key, inside a `tuple` element, and two levels deep. A three-member rotation
# and a parenthesised-against-flat pair are in there too, since a
# swap-only shortcut would pass the two-member cases and fail those.
#
# NO `# cythonpp:` LINE, deliberately: semantic_corpus_test.cpp reads that as
# "cythonpp must report ZERO diagnostics", which is the hardest pin available
# and the one this defect could not have survived.
FLAG = True

if FLAG:
    def module_scope(a: int | str) -> None:
        print(a)

else:
    def module_scope(a: str | int) -> None:
        print(a)


def in_a_parameter(c: bool) -> None:
    if c:
        def g(a: int | None) -> None:
            print(a)

    else:
        def g(a: None | int) -> None:
            print(a)

    g(1)


def in_the_return_type(c: bool) -> int | str:
    def g() -> int | str:
        return 0

    if c:
        def g() -> str | int:
            return "s"

    return g()


def inside_a_list_argument(c: bool) -> None:
    if c:
        def g(a: list[int | str]) -> None:
            print(a)

    else:
        def g(a: list[str | int]) -> None:
            print(a)

    g([1])


def inside_a_dict_key(c: bool) -> None:
    if c:
        def g(a: dict[int | str, bool]) -> None:
            print(a)

    else:
        def g(a: dict[str | int, bool]) -> None:
            print(a)

    g({1: True})


def inside_a_tuple_element(c: bool) -> None:
    if c:
        def g(a: tuple[int | str, bool]) -> None:
            print(a)

    else:
        def g(a: tuple[str | int, bool]) -> None:
            print(a)

    g((1, True))


def two_levels_deep(c: bool) -> None:
    if c:
        def g(a: list[dict[int | str, list[bool | float]]]) -> None:
            print(a)

    else:
        def g(a: list[dict[str | int, list[float | bool]]]) -> None:
            print(a)

    g([])


def a_three_member_rotation(c: bool) -> None:
    if c:
        def g(a: int | str | float) -> None:
            print(a)

    else:
        def g(a: float | int | str) -> None:
            print(a)

    g(1)


def parenthesised_against_flat(c: bool) -> None:
    if c:
        def g(a: int | (str | float)) -> None:
            print(a)

    else:
        def g(a: float | str | int) -> None:
            print(a)

    g(1)


module_scope(1)
in_a_parameter(True)
print(in_the_return_type(True))
inside_a_list_argument(True)
inside_a_dict_key(True)
inside_a_tuple_element(True)
two_levels_deep(True)
a_three_member_rotation(True)
parenthesised_against_flat(True)
