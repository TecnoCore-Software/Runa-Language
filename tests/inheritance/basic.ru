class (Vehicle) [
fun start() [
printr("Vehicle started")
]
]
class (Car : Vehicle) [
]
car = new Car()
car.start()
