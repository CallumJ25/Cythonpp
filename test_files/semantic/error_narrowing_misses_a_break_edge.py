# mypy: error Unsupported operand types for + ("str" and "int") [operator]
# A KNOWN GAP, pinned deliberately: this file has no '# cythonpp:' line, so
# the harness asserts that cythonpp reports NOTHING here. mypy rejects the
# `+` because a `break` can leave `self.n` a str at the loop exit, and the
# post-loop join in TypeChecker::visit(While) models only the end-of-body
# edge, never the break edge. A MISSED error -- the safe direction -- but a
# real one; whoever models break edges should expect this sample to start
# reporting and update it then.
class Bag:
    n: object = object()

    def total(self, flag: bool) -> int:
        self.n = 7
        while flag:
            self.n = "s"
            if flag:
                break
            self.n = 8
        return self.n + 1


print(Bag().total(False))
