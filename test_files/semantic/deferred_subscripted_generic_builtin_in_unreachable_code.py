# mypy: clean
# cpython: clean
# cythonpp: NotImplementedError:17:8 generic builtin type 'type' is not supported
# The round-4 review's `ty1u.py`: `613a460` accepted this program because its
# blanket suppression dropped every "TypeError" inside an unreachable region,
# and `fd616ac` rejected it with
# `TypeError: 'type' is not subscriptable` because round 4 correctly stopped
# suppressing semantic-analyzer judgements -- exposing a judgement that was
# wrong in the first place.
#
# NotImplementedError is the right answer here and is not silent acceptance:
# there is no dead-code elimination, so an unreachable subtree still reaches
# codegen, and a generic this model cannot represent does not become
# representable by being unreachable.
def f() -> None:
    return
    x: type[int] = int
    print(x)


print("module ran")
