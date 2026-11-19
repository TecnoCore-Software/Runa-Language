class (Car) [
private secret = "internal"

fun reveal() [
return self.secret
]
]
c = new Car()
printr(c.reveal())
