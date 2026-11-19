class (Node) [
    partner = null
]
#/ 6000 pairs = 12000 allocations; the runtime collects at 8192
for i = 1 to 6000 [
    a = new Node()
    b = new Node()
    a.partner = b
    b.partner = a
]
printr("survived")
