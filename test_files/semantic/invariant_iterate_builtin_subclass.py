# mypy: clean
class Names(str):
    pass


def initials(names: Names) -> str:
    out = ""
    for ch in names:
        out = out + ch
    return out
