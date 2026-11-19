
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

car1 = new Car("BMW", "M3")
car2 = new Car("Pride", "131")

car1.accelerate(20)
car1.accelerate(22)
car2.accelerate(5)

printr(car1.name, car1.model)
printr("speed:", car1.getSpeed())
printr(car2.name, "speed:", car2.getSpeed())
printr("independent state:", car1.getSpeed() - car2.getSpeed())
