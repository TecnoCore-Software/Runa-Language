class (Box) [
depth = 0
constructor(d) [
self.depth = d
]
fun dig(n) [
if n <= 0 [
return self.depth
]
return new Box(n).dig(n - 1)
]
]
b = new Box(100)
printr(b.dig(100))
