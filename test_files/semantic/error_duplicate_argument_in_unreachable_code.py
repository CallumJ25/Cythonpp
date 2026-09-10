# mypy: error blocking Duplicate argument
# cpython: error SyntaxError: duplicate argument 'x' in function definition
# cythonpp: TypeError:11:5 duplicate argument "x" in function definition
# BOTH ORACLES REJECT THIS ONE, and CPython rejects it at COMPILE time -- the
# file never runs at any reachability, so no reading of reachability can
# excuse silence here. mypy's verdict is a BLOCKING error: exit code 2 with no
# bracketed error code, because its semantic analyzer stopped before type
# checking ever began.
def f() -> None:
    return
    def g(x: int, x: str) -> None:
        pass


print("module ran")
