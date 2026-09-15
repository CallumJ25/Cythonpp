# stdout:
# 6
def template(class_: int, namespace: int) -> int:
    new: int = class_ + namespace
    delete: int = new * 2
    return delete


print(template(1, 2))
