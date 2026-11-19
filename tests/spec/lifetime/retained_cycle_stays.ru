class (Node) [
    partner = null
]
keep1 = new Node()
keep2 = new Node()
keep1.partner = keep2
keep2.partner = keep1
for i = 1 to 6000 [
    a = new Node()
    b = new Node()
    a.partner = b
    b.partner = a
]
printr(keep1 === keep2)
printr(keep1.partner === keep2)
