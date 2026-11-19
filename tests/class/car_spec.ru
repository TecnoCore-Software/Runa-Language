class (Car) [
name = "pride"
model = "saba"
speed = 0

constructor(name, model) [
self.name = name
self.model = model
]

fun accelerate(amount) [
self.speed = self.speed + amount
]

fun getSpeed() [
return self.speed
]
]

car = new Car("BMW", "M3")
car.accelerate(20)
car.accelerate(22)
printr(car.name)
printr(car.model)
printr(car.getSpeed())
