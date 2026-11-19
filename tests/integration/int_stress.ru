fun bump(x) [
    return x + 1
]
c = 0
loop c < 10000 [
    c = bump(c)
]
printr(c)
s = 0
for i = 1 to 10000 [
    s = s + 1
]
printr(s)
