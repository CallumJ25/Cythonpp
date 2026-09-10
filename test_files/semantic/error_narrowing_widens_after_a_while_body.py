# mypy: error Unsupported operand types for + ("str" and "int") [operator]
# cythonpp: NotImplementedError:16:16 operations on a union-typed value require narrowing, which is not supported
# The `while` twin of error_narrowing_widens_after_a_for_target.py:
# TypeChecker::visit(While) has its OWN post-loop join, separate code from
# visit(For)'s, so neither that sample nor any other pinned this one. The
# join must include the ZERO-ITERATION edge (the pre-loop `int`) alongside
# the end-of-body `str`; keeping only the end-of-body edge would report a
# plain TypeError on `str + int` instead of deferring the joined union.
class Bag:
    n: object = object()

    def total(self, flag: bool) -> int:
        self.n = 7
        while flag:
            self.n = "s"
        return self.n + 1


print(Bag().total(False))
