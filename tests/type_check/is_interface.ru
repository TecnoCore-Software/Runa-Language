interface (Movable) [
fun move()
]
class (Car) implements Movable [
fun move() [
printr("vroom")
]
]
c = new Car()
if c is Movable [
printr("movable")
]
if c is Car [
printr("car")
]
