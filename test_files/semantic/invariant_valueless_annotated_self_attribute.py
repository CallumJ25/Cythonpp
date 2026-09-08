# mypy: clean
class Slot:
    def declare(self) -> None:
        self.label: str

    def show(self) -> str:
        return self.label
