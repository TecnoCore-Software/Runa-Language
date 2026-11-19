class (Math) [
fun add(a, b) [
return a + b
]
]
m = new Math()
printr(m.add(2, 3))
printr(m.add(m.add(1, 2), m.add(3, 4)))
printr(m.add(10 * 2, 5 + 5))
