fun countdown(n) [
    loop n > 0 [
        printr(n)
        n = n - 1
    ]
    if n == 0 [
        printr("liftoff")
    ]
]
fun nested(n) [
    if n > 0 [
        nested(n - 1)
    ]
    printr(n)
]
nested(3)
countdown(3)
