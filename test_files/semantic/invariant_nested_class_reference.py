# mypy: clean
class Outer:
    class Inner:
        count: int = 0

        def v(self) -> int:
            return 1


class A:
    class B:
        class C:
            def w(self) -> int:
                return 2


x = Outer.Inner
print(x)
print(Outer.Inner.count)
y = A.B.C()
print(y.w())
