interface (Printable) [
fun print()
]
interface (Movable) [
fun move()
]
class (Car) implements Printable, Movable [
fun print() [
printr("printed")
]
fun move() [
printr("moved")
]
]
car = new Car()
car.print()
car.move()
