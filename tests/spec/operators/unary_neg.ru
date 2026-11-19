#/ Unary minus (DECISIONS #126): -5 is an ordinary expression now. It binds
#/ tighter than * / %, may repeat, may wrap groups, calls and member chains,
#/ and works on floats.
x = -5
printr(x)
printr(-x)
printr(2 * -3)
printr(-(1 + 2))
printr(- -7)
fun negate(n) [
    return -n
]
printr(negate(9))
printr(-1.5)
printr(-2.0 * 3)
