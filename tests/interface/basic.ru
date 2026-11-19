interface (Printable) [
fun print()
]
class (Car) implements Printable [
name = "BMW"

fun print() [
printr(self.name)
]
]
car = new Car()
car.print()
