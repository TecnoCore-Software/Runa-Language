class (P) [
    v = 0
    constructor(a) [
        self.v = a
    ]
    constructor(a, b) [
        self.v = a + b
    ]
]
one = new P(1)
two = new P(2, 3)
printr(one.v)
printr(two.v)
