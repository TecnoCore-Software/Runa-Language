fun sumTo(n) [
    s = 0
    for i = 1 to n [
        s = s + i
    ]
    return s
]
fun tri(n) [
    if n > 1 [
        return tri(n - 1) + sumTo(n)
    ]
    return 1
]
printr(sumTo(4))
printr(tri(4))
