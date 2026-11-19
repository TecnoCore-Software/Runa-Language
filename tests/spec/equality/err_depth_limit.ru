class (A) [
    n = null
]
c = new A()
cur = c
for i = 1 to 80 [
    nxt = new A()
    cur.n = nxt
    cur = nxt
]
d = new A()
cur = d
for i = 1 to 80 [
    nxt = new A()
    cur.n = nxt
    cur = nxt
]
printr(c == d)
