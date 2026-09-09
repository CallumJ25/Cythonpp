# mypy: clean
# cythonpp: NotImplementedError:6:14 a tuple base class is not supported
# mypy accepts this and reveals MyPair()[0] as int, but at runtime MyPair() is
# the empty tuple and MyPair()[0] raises IndexError -- a divergence this
# compiler must not follow, so the construct is named rather than guessed.
class MyPair(tuple[int, str]):
    pass


print(MyPair())
