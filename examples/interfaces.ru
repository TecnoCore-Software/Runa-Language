
interface (Printable) [
fun print()
]

interface (Movable) [
fun move()
]

abstract class (Shape) [
abstract fun area()

fun describe() [
printr("area is", self.area())
]
]

class (Square : Shape) implements Printable, Movable [
side = 0

constructor(side) [
self.side = side
]

fun area() [
return self.side * self.side
]

fun print() [
printr("square with side", self.side)
]

fun move() [
printr("the square cannot move, it is a square")
]
]

s = new Square(5)
s.describe()
s.print()
s.move()
