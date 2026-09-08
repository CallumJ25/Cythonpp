# mypy: clean
class Ghost:
    def __getattr__(self, name: str) -> int:
        return 0


g = Ghost()
print(g.anything)
