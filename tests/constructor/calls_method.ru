class (Car) [
greeting = ""
constructor() [
self.reset()
]
fun reset() [
self.greeting = "ready"
]
]
c = new Car()
printr(c.greeting)
