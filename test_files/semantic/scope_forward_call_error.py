# mypy: error used-before-def
# cythonpp: NameError:4:14 name 'helper' is used before definition
def outer() -> int:
    result = helper()

    def helper() -> int:
        return 1

    return result
