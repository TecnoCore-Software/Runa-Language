class (Box) [
value = 0
constructor(v) [
self.value = v
]
fun get() [
return self.value
]
]
sum = new Box(10).get() + new Box(5).get() * 2
printr(sum)
