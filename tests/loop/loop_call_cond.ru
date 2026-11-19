limit = 3
fun more(n) [
    if n < limit [
        return true
    ]
    return false
]
c = 0
loop more(c) [
    printr(c)
    c = c + 1
]
