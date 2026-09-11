# mypy: clean
# cpython: clean
# The round-4 review's own `rd1.py`, which is where this defect was FOUND:
# `613a460` accepted it because its blanket suppression dropped every
# diagnostic spelling "TypeError" inside an unreachable region, and `fd616ac`
# rejected it because round 4 correctly stopped suppressing semantic-analyzer
# judgements -- exposing a judgement that was wrong to begin with.
#
# The fix is at the root, not by re-masking: the conditional-function
# allowance now applies at function scope too, so the reachable twin
# (invariant_conditional_nested_def_is_not_a_redefinition.py) is clean as
# well. No `# cythonpp:` line -- ZERO diagnostics required.
def f(c: bool) -> None:
    return
    if c:
        def g() -> int:
            return 0

    else:
        def g() -> int:
            return 1


print("module ran")
