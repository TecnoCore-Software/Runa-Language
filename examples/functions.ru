fun add(a, b) [
    result = a + b
    return result
]

x = add(10, 20)
printr(x)

y = add(100, 5)
printr(x)

fun hello() [
    printr("Hello")
]
hello()

fun calculate(a, b) [
    inner = add(a, b)
    return inner * 2
]
printr(calculate(3, 4))

fun check(x) [
    if x == 10 [
        return 100
    ] else [
        return 200
    ]
]
printr(check(10))
printr(check(5))

name = "Runa"
fun greet(who) [
    printr("Hi", who)
]
greet(name)

fun stop() [
    return
]
printr(stop())
