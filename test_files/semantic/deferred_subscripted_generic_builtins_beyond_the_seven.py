# mypy: clean
# cpython: clean
# cythonpp: NotImplementedError:18:8 generic builtin type 'type' is not supported
# cythonpp: NotImplementedError:19:8 generic builtin type 'slice' is not supported
# cythonpp: NotImplementedError:20:8 generic builtin type 'memoryview' is not supported
# cythonpp: NotImplementedError:21:8 generic builtin type 'ExceptionGroup' is not supported
# cythonpp: NotImplementedError:22:8 generic builtin type 'BaseExceptionGroup' is not supported
# The five names the SEVEN-NAME HAND LIST in annotation_resolver.cpp was
# missing, each of which drew a false `TypeError: 'X' is not subscriptable` on
# this mypy-clean, CPython-clean program until the answer became a generated
# field on the class's own row in builtin_class_table.h.
#
# This sample fails the suite by construction if the false positive ever comes
# back: the `# mypy: clean` header makes a TypeError from cythonpp a failure
# on every ctest run, and each `# cythonpp:` line pins the code, the line and
# the column of the answer that replaced it.
def f() -> None:
    a: type[int]
    b: slice[int]
    c: memoryview[int]
    d: ExceptionGroup[ValueError]
    e: BaseExceptionGroup[ValueError]
    print(a, b, c, d, e)
