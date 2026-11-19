class (M) [
fun value() [
return 42
]
fun nothing() [
return
]
fun implicit() [
x = 1
]
]
m = new M()
printr(m.value())
printr(m.nothing())
printr(m.implicit())
