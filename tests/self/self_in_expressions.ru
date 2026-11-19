class (Adder) [
base = 10
fun plus(x) [
return self.base + x
]
]
a = new Adder()
printr(a.plus(5))
printr(a.plus(a.plus(1)))
