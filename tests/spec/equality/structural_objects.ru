class (P) [
    x = 0
]
a = new P()
b = new P()
a.x = 5
b.x = 5
printr(a == b)
printr(a == new P())
