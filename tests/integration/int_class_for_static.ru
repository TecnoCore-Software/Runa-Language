class (Counter) [
    static total = 0
    static fun sumTo(n) [
        s = 0
        for i = 1 to n [
            s = s + i
        ]
        return s
    ]
]
for i = 1 to 3 [
    Counter.total = Counter.total + i
]
printr(Counter.total)
printr(Counter.sumTo(10))
