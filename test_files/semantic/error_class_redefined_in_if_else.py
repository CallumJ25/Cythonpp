# mypy: error Name "Bag" already defined [no-redef]
# cythonpp: TypeError:10:5 name "Bag" already defined on line 6
FLAG = True

if FLAG:
    class Bag:
        pass

else:
    class Bag:
        pass


print(Bag())
