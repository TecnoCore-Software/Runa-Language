
class (Car) [
class (Engine) [
horsepower = 100

fun rev() [
printr("vroom x", self.horsepower)
]
]

fun makeEngine() [
return new Engine()
]
]

car = new Car()
engine = car.makeEngine()
engine.rev()
