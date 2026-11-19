class (Counter) [
fun countdown(n) [
if n > 0 [
printr(n)
self.countdown(n - 1)
]
]
fun fact(n) [
if n <= 1 [
return 1
]
return n * self.fact(n - 1)
]
]
c = new Counter()
c.countdown(3)
printr(c.fact(5))
