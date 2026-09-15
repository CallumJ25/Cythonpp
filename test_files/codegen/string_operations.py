# stdout:
# hello
# hello world
# hellohellohello
# 5
# True
# False
# True
# héllo wörld
# 11
# 3
a: str = "hello"
b: str = "world"
print(a)
print(a + " " + b)
print(a * 3)
print(len(a))
print(a == "hello")
print(a == b)
print(a < b)
non_ascii: str = "héllo wörld"
print(non_ascii)
print(len(non_ascii))
print(len("日本語"))
