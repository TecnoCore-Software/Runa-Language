fun countdown(n) [
    if n == 0 [
        return 0
    ] else [
        return countdown(n - 1)
    ]
]
printr(countdown(5))

fun fact(n) [
    if n <= 1 [
        return 1
    ]
    return fact(n - 1) * n
]
printr(fact(5))

fun fib(n) [
    if n < 2 [
        return n
    ]
    return fib(n - 1) + fib(n - 2)
]
printr(fib(10))

fun sumTo(n) [
    if n == 0 [
        return 0
    ]
    return n + sumTo(n - 1)
]
printr(sumTo(5))
