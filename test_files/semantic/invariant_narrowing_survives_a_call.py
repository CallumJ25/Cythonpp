# mypy: clean
# cpython: error TypeError: can only concatenate str (not "int") to str
# Narrowing survives a call that provably invalidates it -- and this sample is
# the case the optional '# cpython:' label exists for. mypy --strict accepts
# the file (`self.n` stays `int` across `self.reset()`, so `self.n + 1` is
# fine), while running it really does raise, because `reset` really did
# replace the `int` with a `str`. That is not a violation of the union rule:
# the rule is about whether a program can be IMPORTED, and no static checker
# -- mypy included, which is clean here -- claims every runtime path is
# exception-free. The label is here so the fact is recorded in the file rather
# than left to be rediscovered, and so --check-corpus pins it.
class Bag:
    n: object = object()

    def reset(self) -> None:
        self.n = "reset"

    def bump(self) -> int:
        self.n = 7
        self.reset()
        return self.n + 1


print(Bag().bump())
