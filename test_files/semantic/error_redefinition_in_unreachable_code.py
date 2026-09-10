# mypy: error no-redef
# cpython: clean
# cythonpp: TypeError:12:5 name "y" already defined on line 11
# mypy's SEMANTIC ANALYZER runs in unreachable code; only its TYPE CHECKER
# stops. So a redefinition after a `return` is still an error mypy reports,
# even though a bad operand type in the same position is not. Round 3 of the
# unreachable-code work suppressed the CODE STRING "TypeError", which cythonpp
# spells for both kinds of judgement, and silently accepted this program.
def f() -> None:
    return
    y: int = 1
    y: str = "s"


print("module ran")
