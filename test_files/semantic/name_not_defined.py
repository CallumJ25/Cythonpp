# mypy: error name-defined
# cythonpp: NameError:4:13 name 'z' is not defined
def f(x: int) -> int:
    y = x + z
    # NOTE: real mypy --strict reports TWO errors on the next line -- the
    # name-defined error above (from computing `y`, reported at its own
    # site), plus a second "no-any-return" error here because `y`'s type
    # collapses to Any/Unknown and the declared return type is `int`. Our
    # label above only expects the one NameError because cythonpp has no
    # no-any-return rule yet -- do not "fix" this to expect two diagnostics
    # without first adding that rule; the current single-diagnostic count is
    # a known, deliberate gap, not a bug in this sample.
    return y
