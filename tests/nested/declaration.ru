class (Car) [
class (Engine) [
horsepower = 100
]
fun makeEngine() [
return new Engine()
]
]
car = new Car()
engine = car.makeEngine()
printr(engine.horsepower)
