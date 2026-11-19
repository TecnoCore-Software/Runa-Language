class (Vehicle) [
wheels = 4
fuel = "gas"
]
class (Car : Vehicle) [
]
car = new Car()
printr(car.wheels)
printr(car.fuel)
