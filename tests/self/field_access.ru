class (Counter) [
count = 0

fun increment() [
self.count = self.count + 1
]

fun value() [
return self.count
]
]
c = new Counter()
c.increment()
c.increment()
c.increment()
printr(c.value())
