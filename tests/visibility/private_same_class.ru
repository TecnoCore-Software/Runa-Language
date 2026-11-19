class (Car) [
private secret = 1

fun peek(other) [
return other.secret
]
]
a = new Car()
b = new Car()
printr(a.peek(b))
