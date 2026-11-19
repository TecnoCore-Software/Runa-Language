class (Node) [
value = 0
constructor(v) [
self.value = v
]
fun get() [
return self.value
]
]
total = new Node(1).get() + new Node(2).get() + new Node(3).get()
printr(total)
